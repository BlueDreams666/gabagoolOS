/* GabagoolOS keyboard. GPLv2+. Included by sloppy.c before its first user.
 *
 * One keyboard for every field: a bar of three suggestions over four rows
 * of keys, in the layout of the language being typed. The globe key flips
 * between the two keyboards chosen in Settings > Keyboards, and the space
 * bar names the one in use.
 *
 * One touch surface covers the keys, so a finger is always on the nearest
 * key (no dead gaps), it can slide to the right one before lifting, and a
 * path across several letters is read as a glided word
 * (sloppy_swipe_type.h). Holding a letter offers its accents (and, on the
 * top row, its digit); holding Delete repeats it, then takes whole words;
 * a finger dragged along the space bar moves the cursor. Pinyin turns
 * letters into hanzi from a candidate bar (sloppy_pinyin.h), and romaji
 * into kana (and a few kanji words) the same way (sloppy_kana.h). The phone
 * keypad (T9) types with twelve keys: a press a word from the dictionary,
 * or each key pressed until its letter comes up (P58). */
#include "sloppy_swipe_type.h"
#include "sloppy_kb_dict.h"
#include "sloppy_unshape.h"
#include "sloppy_pinyin.h"
#include "sloppy_kana.h"
#include "src/misc/lv_text_ap.h"         /* joining Arabic in a hint, as LVGL does in a label */

enum { KB_EN, KB_ES, KB_FR, KB_DE, KB_HE, KB_AR, KB_ZH, KB_IT, KB_PT, KB_NL, KB_PL, KB_CS, KB_RU, KB_T9, KB_JA, KB_N };   /* saved as KB_* + 1: new ones go at the end */
#define KBF_RTL    1            /* the words run from the right */
#define KBF_CASE   2            /* letters have capitals: a shift key */
#define KBF_PINYIN 4            /* letters spell sounds, the bar turns them into hanzi */
#define KBF_T9     8            /* the phone keypad: twelve keys, three or four letters each */
#define KBF_KANA   16           /* letters spell romaji, the bar turns them into kana */
#define KBF_COMPOSE (KBF_PINYIN | KBF_KANA)
struct kb_def {
    const char *name;           /* on the space bar and in Settings, as the language writes it */
    const char *rows[3];        /* the letters, a key each */
    const char *abc;            /* the key back from the symbols */
    const char *punct;          /* the symbols' third row */
    uint8_t flags;
    const char *dict;
};
static const struct kb_def kb_defs[KB_N] = {
    [KB_EN] = { "English", { "qwertyuiop", "asdfghjkl", "zxcvbnm" }, "ABC", ".,?!'", KBF_CASE, kb_dict_en },
    [KB_ES] = { "Espa\xc3\xb1ol", { "qwertyuiop", "asdfghjkl\xc3\xb1", "zxcvbnm" }, "ABC", ".,?!'", KBF_CASE, kb_dict_es },
    [KB_FR] = { "Fran\xc3\xa7" "ais", { "azertyuiop", "qsdfghjklm", "wxcvbn" }, "ABC", ".,?!'", KBF_CASE, kb_dict_fr },
    [KB_DE] = { "Deutsch", { "qwertzuiop\xc3\xbc", "asdfghjkl\xc3\xb6\xc3\xa4", "yxcvbnm" }, "ABC", ".,?!'", KBF_CASE, kb_dict_de },
    [KB_HE] = { "\xd7\xa2\xd7\x91\xd7\xa8\xd7\x99\xd7\xaa",
                { "\xd7\xa7\xd7\xa8\xd7\x90\xd7\x98\xd7\x95\xd7\x9f\xd7\x9d\xd7\xa4",
                  "\xd7\xa9\xd7\x93\xd7\x92\xd7\x9b\xd7\xa2\xd7\x99\xd7\x97\xd7\x9c\xd7\x9a\xd7\xa3",
                  "\xd7\x96\xd7\xa1\xd7\x91\xd7\x94\xd7\xa0\xd7\x9e\xd7\xa6\xd7\xaa\xd7\xa5" },
                "\xd7\x90\xd7\x91\xd7\x92", ".,?!'", KBF_RTL, kb_dict_he },
    [KB_AR] = { "\xd8\xa7\xd9\x84\xd8\xb9\xd8\xb1\xd8\xa8\xd9\x8a\xd8\xa9",
                { "\xd8\xb6\xd8\xb5\xd8\xab\xd9\x82\xd9\x81\xd8\xba\xd8\xb9\xd9\x87\xd8\xae\xd8\xad\xd8\xac",
                  "\xd8\xb4\xd8\xb3\xd9\x8a\xd8\xa8\xd9\x84\xd8\xa7\xd8\xaa\xd9\x86\xd9\x85\xd9\x83\xd8\xb7",
                  "\xd8\xb8\xd8\xb0\xd8\xaf\xd8\xb2\xd8\xb1\xd9\x88\xd8\xa9\xd9\x89\xd8\xa1" },
                "\xd8\xa3\xd8\xa8\xd8\xac", ".\xd8\x8c\xd8\x9f!'", KBF_RTL, kb_dict_ar },
    [KB_ZH] = { "\xe6\x8b\xbc\xe9\x9f\xb3", { "qwertyuiop", "asdfghjkl", "zxcvbnm" }, "\xe6\x8b\xbc",
                "\xef\xbc\x8c\xe3\x80\x82\xef\xbc\x9f\xef\xbc\x81\xe3\x80\x81", KBF_PINYIN, NULL },
    [KB_IT] = { "Italiano", { "qwertyuiop", "asdfghjkl", "zxcvbnm" }, "ABC", ".,?!'", KBF_CASE, kb_dict_it },
    [KB_PT] = { "Portugu\xc3\xaas", { "qwertyuiop", "asdfghjkl\xc3\xa7", "zxcvbnm" }, "ABC", ".,?!'", KBF_CASE, kb_dict_pt },
    [KB_NL] = { "Nederlands", { "qwertyuiop", "asdfghjkl", "zxcvbnm" }, "ABC", ".,?!'", KBF_CASE, kb_dict_nl },
    [KB_PL] = { "Polski", { "qwertyuiop", "asdfghjkl", "zxcvbnm" }, "ABC", ".,?!'", KBF_CASE, kb_dict_pl },
    [KB_CS] = { "\xc4\x8c" "e\xc5\xa1tina", { "qwertzuiop", "asdfghjkl", "yxcvbnm" }, "ABC", ".,?!'", KBF_CASE, kb_dict_cs },
    [KB_RU] = { "\xd0\xa0\xd1\x83\xd1\x81\xd1\x81\xd0\xba\xd0\xb8\xd0\xb9",
                { "\xd0\xb9\xd1\x86\xd1\x83\xd0\xba\xd0\xb5\xd0\xbd\xd0\xb3\xd1\x88\xd1\x89\xd0\xb7\xd1\x85",
                  "\xd1\x84\xd1\x8b\xd0\xb2\xd0\xb0\xd0\xbf\xd1\x80\xd0\xbe\xd0\xbb\xd0\xb4\xd0\xb6\xd1\x8d",
                  "\xd1\x8f\xd1\x87\xd1\x81\xd0\xbc\xd0\xb8\xd1\x82\xd1\x8c\xd0\xb1\xd1\x8e" },
                "\xd0\x90\xd0\x91\xd0\x92", ".,?!'", KBF_CASE, kb_dict_ru },
    [KB_T9] = { "T9", { "", "", "" }, "abc", ".,?!'", KBF_CASE | KBF_T9, NULL },
    [KB_JA] = { "日本語", { "qwertyuiop", "asdfghjkl", "zxcvbnm" }, "あいう", "、。？！ー", KBF_KANA, NULL },
};
/* the keyboard a language types with */
static int kb_of_language(int lang)
{
    static const int k[] = { KB_EN, KB_ES, KB_FR, KB_DE, KB_HE, KB_AR, KB_ZH, KB_IT, KB_NL, KB_PT, KB_PL, KB_CS, KB_RU, KB_JA };
    return lang > 0 && lang < (int)ARRAYLEN(k) ? k[lang] : KB_EN;
}
/* the two the globe key flips between: chosen in Settings > Keyboards, or
 * the language's and English (Hebrew beside English, as the search
 * keyboard always had) */
static int kb_chosen(int slot)
{
    int v = slot ? global_settings.sloppy_kb_second : global_settings.sloppy_kb_first;
    if (v >= 1 && v <= KB_N) return v - 1;
    int first = kb_of_language(global_settings.sloppy_language);
    if (!slot) return first;
    return first == KB_EN ? KB_HE : KB_EN;
}

/* Accents and other letters a key holds, the commonest first. The top
 * row's keys hold their digit too (q 1 ... p 0). */
static const char *kb_accents(uint32_t cp)
{
    switch (cp)
    {
    case 'a': return "\xc3\xa1\xc3\xa0\xc3\xa2\xc3\xa4\xc3\xa3\xc3\xa5\xc4\x85\xc3\xa6\xc4\x81";
    case 'c': return "\xc3\xa7\xc4\x8d\xc4\x87";
    case 'd': return "\xc4\x8f";
    case 'e': return "\xc3\xa9\xc3\xa8\xc3\xaa\xc3\xab\xc4\x99\xc4\x9b\xc4\x93\xc4\x97";
    case 'i': return "\xc3\xad\xc3\xac\xc3\xae\xc3\xaf\xc4\xab\xc4\xaf";
    case 'l': return "\xc5\x82";
    case 'n': return "\xc3\xb1\xc5\x84\xc5\x88";
    case 'o': return "\xc3\xb3\xc3\xb2\xc3\xb4\xc3\xb6\xc3\xb5\xc3\xb8\xc5\x93\xc5\x8d";
    case 'r': return "\xc5\x99";
    case 's': return "\xc3\x9f\xc5\x9b\xc5\xa1";
    case 't': return "\xc5\xa5";
    case 'u': return "\xc3\xba\xc3\xb9\xc3\xbb\xc3\xbc\xc5\xab\xc5\xaf";
    case 'y': return "\xc3\xbd\xc3\xbf";
    case 'z': return "\xc5\xbe\xc5\xba\xc5\xbc";
    case 0x435: return "\xd1\x91";               /* е: ё */
    case 0x44c: return "\xd1\x8a";               /* ь: ъ */
    case 0x627: return "\xd8\xa3\xd8\xa5\xd8\xa2";   /* ا: أ إ آ */
    case 0x648: return "\xd8\xa4";               /* و: ؤ */
    case 0x64a: return "\xd8\xa6";               /* ي: ئ */
    case 0x649: return "\xd8\xa6";               /* ى: ئ */
    case 0x5d2: return "\xd7\x92'";              /* ג: ג' */
    case '.': return ",?!:;-";
    default: return NULL;
    }
}

/* ------------------------------------------------------------ geometry */
#define KB_PAD_X   4            /* screen edge to the first key */
#define KB_GAP     5            /* key to key, across */
#define KB_ROW_GAP 8            /* row to row */
#define KB_KEY_H   56           /* a key's height: a fingertip on the R1 */
#define KB_BAR_H   48           /* the suggestions */
#define KB_TOP     4
#define KB_BOTTOM  8
#define KB_H (KB_TOP + KB_BAR_H + 4 * KB_KEY_H + 3 * KB_ROW_GAP + KB_BOTTOM)
#define KB_KEYS_MAX 56
/* A keyboard keeps its size whatever Text size says, as a phone's does:
 * its keys cannot grow, so neither do their letters (sl_fonts[0]). */
#define KB_PATH_MAX 200

enum { KA_CHAR, KA_SHIFT, KA_BKSP, KA_SPACE, KA_MODE, KA_SYMS, KA_GLOBE, KA_ENTER, KA_DRAW, KA_HIDE, KA_T9, KA_T9MODE };
enum { KB_TEXT, KB_NAME, KB_PASSWORD, KB_SEARCH };
struct kb_key { lv_obj_t *o, *l; uint32_t cp; uint8_t act; uint8_t row, col; };

struct kb {
    lv_obj_t *root, *bar, *keys, *pop, *trail[2], *acc;
    lv_obj_t *ta;
    int kind;
    int w;                      /* its width: the screen's, or a sheet's inside */
    lv_event_cb_t enter;
    const char *enter_label;
    int lay, page;              /* the keyboard in use; 0 letters, 1 and 2 symbols */
    int shift;                  /* 0 off, 1 the next letter, 2 locked */
    struct kb_key k[KB_KEYS_MAX];
    int nk;
    /* the finger */
    float px[KB_PATH_MAX], py[KB_PATH_MAX];
    int pn, hot;
    bool gliding, cursoring, accenting, down;
    int acc_n, acc_sel, acc_x0;    /* the accents' row: how many, the one chosen, where the finger was when it opened */
    uint32_t acc_cp[16];
    int cur_x;                  /* where the cursor last moved, on the space bar */
    uint32_t held_at, next_at;  /* Delete held: when it went down, when it deletes next */
    int deleted;
    lv_timer_t *timer;
    lv_point_precise_t trail_pts[64];
    /* words */
    struct st_layout geo;
    struct st_vocab voc;
    int voc_lay;                /* the layout the words were read for */
    int voc_more;               /* the library words still to read (search), -1 none */
    bool (*more)(struct kb *kb);
    /* the bar */
    char sug[3][ST_WORD_MAX * 3 + 1];
    int nsug;
    bool sug_glide;             /* the bar holds a glide's runners-up: a pick replaces the word */
    bool sug_letter;            /* the bar holds a drawn letter's runners-up: a pick replaces it */
    lv_obj_t *sug_o[3], *sep[2];
    /* pinyin: the letters typed, the candidates for them */
    char py_buf[32];
    lv_obj_t *py_row;
    const char *py_c[24];
    uint8_t py_clen[24], py_used[24];
    int py_n;
    char ja_c[2][96];            /* romaji as hiragana and katakana, for the bar */
    void (*on_text)(struct kb *kb);   /* after the words changed (search runs) */
    void (*on_draw)(struct kb *kb);   /* the Draw key (search) */
    /* the phone keypad: predictive, the keys pressed for the word and what
     * they spell now; multi-tap, the key going round its letters */
    char t9_seq[ST_WORD_MAX + 1];
    char t9_cand[8][ST_WORD_MAX * 3 + 1];
    int t9_n, t9_sel, t9_shown;   /* candidates, the one shown, its length in the field (code points) */
    bool t9_cap, t9_held, sug_t9;
    int t9_last, t9_idx;          /* multi-tap: the key, which of its letters */
    uint32_t t9_at;
    /* a page's own colours (the pet's LCD): tray, keys, pressed, words; 0 radius when square */
    bool pal;
    uint32_t pal_tray, pal_key, pal_press, pal_ink;
    int pal_radius;
};
static struct kb *kb_live;            /* the keyboard of the page on top, for the tests */
static uint32_t kb_built_us;          /* how long the last keyboard took to build */

static const struct kb_def *kb_def_now(const struct kb *kb) { return &kb_defs[kb->lay]; }
static bool kb_glides(const struct kb *kb)
{
    return global_settings.sloppy_swipe_typing && kb->kind != KB_PASSWORD && kb->page == 0 && kb_def_now(kb)->dict;
}
static bool kb_suggests(const struct kb *kb) { return kb->kind != KB_PASSWORD && kb_def_now(kb)->dict; }
static bool kb_two(void) { return kb_chosen(0) != kb_chosen(1); }

/* ------------------------------------------------------------ the field */
/* A field's words as typed. LVGL shapes Arabic as it stores a label's
 * text, so a field holds presentation forms (ﺣ for ح): read back, they
 * are the letters again, as names, searches and files need them. */
static const char *sl_unshape(const char *in)
{
    static char buf[4][2048];          /* the longest field (600 letters) read back whole */
    static int k;
    k = (k + 1) & 3;
    char *o = buf[k], *end = buf[k] + sizeof buf[k] - 7;
    while (*in && o < end)
    {
        const char *at = in;
        uint32_t c = st_utf8_next(&in);
        int lo = 0, hi = (int)ARRAYLEN(sl_unshape_map) - 1, f = -1;
        if (c >= 0xfb50 && c <= 0xfeff)
            while (lo <= hi) { int m = (lo + hi) / 2; if (sl_unshape_map[m][0] == c) { f = m; break; } if (sl_unshape_map[m][0] < c) lo = m + 1; else hi = m - 1; }
        if (f < 0) { memcpy(o, at, in - at); o += in - at; continue; }
        o += st_utf8_put(o, sl_unshape_map[f][1]);
        if (sl_unshape_map[f][2]) o += st_utf8_put(o, sl_unshape_map[f][2]);
    }
    *o = '\0';
    return buf[k];
}
static const char *sl_ta_text(lv_obj_t *ta) { return sl_unshape(lv_textarea_get_text(ta)); }
/* A field's limit counts letters, its buffer bytes: what is copied out
 * is cut back to a whole letter (sloppy_swipe.inc) */
static void pl_utf8_trim(char *s);
/* A field's hint. LVGL joins Arabic letters in a label's text but not in
 * a hint: join them here, as the words would be once typed. */
static void sl_ta_hint(lv_obj_t *ta, const char *s)
{
    char b[256];
    if (sl_has_arabic(s) && lv_text_ap_calc_bytes_count(s) < sizeof b) { lv_text_ap_proc(s, b); s = b; }
    lv_textarea_set_placeholder_text(ta, s);
}

/* the field's words and where the cursor is in them, in bytes */
static const char *kb_text(const struct kb *kb) { return kb->ta ? lv_textarea_get_text(kb->ta) : ""; }
static int kb_cursor_byte(const struct kb *kb)
{
    const char *t = kb_text(kb);
    uint32_t pos = kb->ta ? lv_textarea_get_cursor_pos(kb->ta) : 0;
    int b = 0;
    for (uint32_t i = 0; i < pos && t[b]; i++) { b++; while ((t[b] & 0xc0) == 0x80) b++; }
    return b;
}
/* the word the cursor stands at the end of: its first byte */
static int kb_word_start(const struct kb *kb, int end)
{
    const char *t = kb_text(kb);
    int s = end;
    while (s > 0 && t[s - 1] != ' ' && t[s - 1] != '\n') s--;
    return s;
}
static void kb_changed(struct kb *kb);
/* the letters that went in: fewer than s has when the field is full */
static int kb_put(struct kb *kb, const char *s)
{
    if (!kb->ta) return 0;
    uint32_t at = lv_textarea_get_cursor_pos(kb->ta);
    lv_textarea_add_text(kb->ta, s);
    return (int)(lv_textarea_get_cursor_pos(kb->ta) - at);
}
static void kb_del(struct kb *kb, int n)
{
    if (!kb->ta) return;
    while (n-- > 0) lv_textarea_delete_char(kb->ta);
}
/* how many letters (code points) from byte a to byte b */
static int kb_cps(const char *t, int a, int b)
{
    int n = 0;
    for (int i = a; i < b; i++) if ((t[i] & 0xc0) != 0x80) n++;
    return n;
}

/* ------------------------------------------------------------ the look */
static uint32_t kb_key_bg(int act) { return act == KA_CHAR || act == KA_SPACE || act == KA_T9 ? C_CARD : C_CARD_P; }
static lv_obj_t *kb_key_obj(struct kb *kb, int w, int h)
{
    lv_obj_t *o = box(kb->keys);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, kb->pal ? kb->pal_radius : sl_zune() ? 0 : SL_R_SM, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    /* light: a key stands on the tray by a hairline of shade, as paper keys do */
    if (sl_theme() == 1)
    {
        lv_obj_set_style_shadow_width(o, 0, 0);
        lv_obj_set_style_shadow_offset_y(o, 1, 0);
        lv_obj_set_style_shadow_spread(o, 0, 0);
        lv_obj_set_style_shadow_color(o, lv_color_hex(C_DIM), 0);
        lv_obj_set_style_shadow_opa(o, LV_OPA_50, 0);
    }
    return o;
}
/* A key's face, from the theme's own colours: letters on the card
 * colour, the keys that do things a step back (in light and black, the
 * secondary ink washed thin over the tray), Enter in the accent. Pressed,
 * a letter sinks to the tray's step and a function key lifts. */
static void kb_key_paint(struct kb *kb, int i, bool pressed)
{
    struct kb_key *k = &kb->k[i];
    bool letter = k->act == KA_CHAR || k->act == KA_SPACE || k->act == KA_T9 || (k->act == KA_SHIFT && kb->shift);
    uint32_t bg;
    uint8_t opa = LV_OPA_COVER;
    if (k->act == KA_ENTER) { bg = C_ACCENT; if (pressed) opa = LV_OPA_70; }
    else if (sl_theme() == 1)
    {
        if (letter) bg = pressed ? C_CARD_P : C_CARD;
        else { bg = C_SUB; opa = pressed ? 80 : 46; }
    }
    else if (sl_theme() == 2)            /* True dark and Zule: keys a step up from black */
    {
        if (letter && !pressed) bg = C_CARD_P;
        else { bg = C_SUB; opa = pressed ? 80 : 46; }
    }
    else if (letter) bg = pressed ? C_CARD_P : C_CARD;
    else bg = pressed ? C_CARD : C_CARD_P;
    if (kb->pal) { bg = pressed ? kb->pal_press : kb->pal_key; opa = LV_OPA_COVER; }
    lv_obj_set_style_bg_color(k->o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(k->o, opa, 0);
    if (kb->pal && k->l && !lv_obj_check_type(k->l, &lv_obj_class))
        lv_obj_set_style_text_color(k->l, lv_color_hex(pressed ? kb->pal_key : kb->pal_ink), 0);
}
/* a key that does something, outside the key grid (Draw's column): as kb_key_paint */
static void kb_fn_face(lv_obj_t *o)
{
    bool wash = sl_theme() == 1 || sl_theme() == 2;
    lv_obj_set_style_bg_color(o, lv_color_hex(wash ? C_SUB : C_CARD_P), 0);
    lv_obj_set_style_bg_opa(o, wash ? 46 : LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(wash ? C_SUB : C_CARD), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(o, wash ? 80 : LV_OPA_COVER, LV_STATE_PRESSED);
}
static uint32_t kb_case(const struct kb *kb, uint32_t cp)
{
    return kb->shift && (kb_def_now(kb)->flags & KBF_CASE) ? st_upper(cp) : cp;
}
static void t9_label(struct kb *kb, int i);
static void kb_label_key(struct kb *kb, int i)
{
    struct kb_key *k = &kb->k[i];
    if (k->act == KA_T9) { t9_label(kb, i); return; }
    if (k->act != KA_CHAR || !k->l) return;
    char s[8];
    s[st_utf8_put(s, kb_case(kb, k->cp))] = '\0';
    label_if_changed(k->l, s);
}
static void kb_shift_icon(struct kb *kb)
{
    for (int i = 0; i < kb->nk; i++)
        if (kb->k[i].act == KA_SHIFT && kb->k[i].l)
        {
            lv_obj_remove_event_cb(kb->k[i].l, sl_icon_draw);
            lv_obj_add_event_cb(kb->k[i].l, sl_icon_draw, LV_EVENT_DRAW_MAIN,
                                (void *)(intptr_t)(kb->shift == 2 ? IC_CAPS : kb->shift ? IC_SHIFT_ON : IC_SHIFT));
            lv_obj_invalidate(kb->k[i].l);
            kb_key_paint(kb, i, false);
        }
}
static void kb_set_shift(struct kb *kb, int s)
{
    if (!(kb_def_now(kb)->flags & KBF_CASE) || kb->page) s = 0;
    if (s == kb->shift) return;
    kb->shift = s;
    for (int i = 0; i < kb->nk; i++) kb_label_key(kb, i);
    kb_shift_icon(kb);
}
/* a capital for the next letter: an empty name, or a new sentence */
static void kb_autocap(struct kb *kb)
{
    if (kb->shift == 2 || kb->kind != KB_NAME) return;
    const char *t = kb_text(kb);
    int b = kb_cursor_byte(kb);
    bool cap = b == 0;
    if (b >= 2 && t[b - 1] == ' ' && (t[b - 2] == '.' || t[b - 2] == '!' || t[b - 2] == '?')) cap = true;
    kb_set_shift(kb, cap ? 1 : 0);
}

/* ------------------------------------------------------------ the bar */
static void kb_sug_cb(lv_event_t *e);
static void kb_bar_clear(struct kb *kb)
{
    kb->nsug = 0;
    kb->sug_glide = kb->sug_letter = false;
    for (int i = 0; i < 3; i++) if (kb->sug_o[i]) label_if_changed(lv_obj_get_child(kb->sug_o[i], 0), "");
    for (int i = 0; i < 2; i++) if (kb->sep[i]) lv_obj_add_flag(kb->sep[i], LV_OBJ_FLAG_HIDDEN);
}
/* the three in the bar: the best in the middle, as the eye lands there */
static void kb_bar_show(struct kb *kb)
{
    static const int slot[3] = { 1, 0, 2 };
    for (int i = 0; i < 3; i++) if (kb->sug_o[i]) label_if_changed(lv_obj_get_child(kb->sug_o[i], 0), "");
    for (int i = 0; i < kb->nsug && i < 3; i++)
        if (kb->sug_o[slot[i]]) label_if_changed(lv_obj_get_child(kb->sug_o[slot[i]], 0), kb->sug[i]);
    for (int i = 0; i < 2; i++) if (kb->sep[i]) lv_obj_update_flag(kb->sep[i], LV_OBJ_FLAG_HIDDEN, kb->nsug < 2);
}

/* ------------------------------------------------------------ words */
static void kb_layout_geo(struct kb *kb)
{
    kb->geo.n = 0;
    kb->geo.key_w = 40;
    kb->geo.key_h = KB_KEY_H;
    for (int i = 0; i < kb->nk && kb->geo.n < ST_KEYS; i++)
    {
        if (kb->k[i].act != KA_CHAR || kb->k[i].row > 2) continue;   /* the letters, not the bottom row's full stop */
        lv_area_t a;
        lv_obj_get_coords(kb->k[i].o, &a);
        kb->geo.k[kb->geo.n++] = (struct st_key){ kb->k[i].cp, (a.x1 + a.x2) / 2.0f, (a.y1 + a.y2) / 2.0f };
        if (kb->k[i].row == 0) kb->geo.key_w = (float)lv_area_get_width(&a) + KB_GAP;   /* a letter's pitch */
        kb->geo.key_h = (float)lv_area_get_height(&a) + KB_ROW_GAP;
    }
}
static void kb_voc_free(struct kb *kb)
{
    free(kb->voc.w); free(kb->voc.arena); free(kb->voc.hash);
    memset(&kb->voc, 0, sizeof kb->voc);
    kb->voc_lay = -1;
}
#define KB_VOC_WORDS 16000
#define KB_VOC_ARENA (160 * 1024)
/* The words of the keyboard in use: its dictionary at once (a few
 * thousand), and for search the library's in steps (kb->more). */
static void kb_voc_begin(struct kb *kb)
{
    if (!kb_suggests(kb) || (kb->voc_lay == kb->lay && kb->voc.w)) return;
    kb_voc_free(kb);
    kb_layout_geo(kb);
    int words = kb->more ? KB_VOC_WORDS : 4000, arena = kb->more ? KB_VOC_ARENA : 40 * 1024;
    kb->voc.max = words; kb->voc.size = arena; kb->voc.cap = words * 2;
    kb->voc.w = malloc(words * sizeof *kb->voc.w);
    kb->voc.arena = malloc(arena);
    kb->voc.hash = malloc(kb->voc.cap * sizeof *kb->voc.hash);
    if (!kb->voc.w || !kb->voc.arena || !kb->voc.hash) { kb_voc_free(kb); return; }
    for (int i = 0; i < kb->voc.cap; i++) kb->voc.hash[i] = -1;
    kb->voc_lay = kb->lay;
    st_vocab_add_ranked(&kb->geo, &kb->voc, kb_def_now(kb)->dict, 2000);
    kb->voc_more = kb->more ? 0 : -1;
    if (kb->voc_more < 0) { free(kb->voc.hash); kb->voc.hash = NULL; kb->voc.cap = 0; }
}
static bool kb_voc_building(const struct kb *kb) { return kb->voc.w && kb->voc_more >= 0; }
static void kb_voc_step(struct kb *kb)
{
    if (!kb_voc_building(kb)) return;
    if (!kb->more(kb))
    {
        sl_log("keyboard: %d words for %s, %d KB", kb->voc.n, kb_defs[kb->lay].name,
               (int)((kb->voc.n * sizeof *kb->voc.w + kb->voc.used) / 1024));
        free(kb->voc.hash); kb->voc.hash = NULL;
        kb->voc.cap = 0;
        kb->voc_more = -1;
    }
}

/* the word being typed, in the bar's three: as typed, then the commonest
 * words it starts */
static void kb_suggest(struct kb *kb)
{
    if (kb->sug_glide || kb->sug_letter || kb->sug_t9) return;   /* the keypad keeps its own words there */
    kb_bar_clear(kb);
    if (!kb_suggests(kb) || !kb->voc.w || kb->page) return;
    const char *t = kb_text(kb);
    int end = kb_cursor_byte(kb), start = kb_word_start(kb, end);
    if (end - start < 1 || end - start > ST_WORD_MAX * 3) return;
    char typed[ST_WORD_MAX * 3 + 1];
    memcpy(typed, t + start, end - start);
    typed[end - start] = '\0';
    strlcpy(typed, sl_unshape(typed), sizeof typed);
    int found[3];
    int n = st_complete(&kb->voc, typed, found, 3);
    bool upper = (kb_def_now(kb)->flags & KBF_CASE) && st_lower((uint32_t)(unsigned char)typed[0]) != (uint32_t)(unsigned char)typed[0];
    for (int i = 0; i < n && kb->nsug < 3; i++)
    {
        const char *w = kb->voc.arena + kb->voc.w[found[i]].off;
        char *o = kb->sug[kb->nsug];
        strlcpy(o, w, sizeof kb->sug[0]);
        if (upper) { const char *q = w; uint32_t c = st_utf8_next(&q); int l = st_utf8_put(o, st_upper(c)); strlcpy(o + l, q, sizeof kb->sug[0] - l); }
        if (!strcmp(o, typed)) continue;
        kb->nsug++;
    }
    /* the word as typed stays a choice beside them */
    if (kb->nsug)
    {
        if (kb->nsug == 3) kb->nsug = 2;
        memmove(kb->sug[1], kb->sug[0], sizeof kb->sug[0] * kb->nsug);
        strlcpy(kb->sug[0], typed, sizeof kb->sug[0]);
        /* the commonest completion in the middle, what was typed beside it */
        char tmp[sizeof kb->sug[0]];
        memcpy(tmp, kb->sug[0], sizeof tmp); memcpy(kb->sug[0], kb->sug[1], sizeof tmp); memcpy(kb->sug[1], tmp, sizeof tmp);
        kb->nsug++;
    }
    kb_bar_show(kb);
}

/* a word glided: in the field, the runners-up in the bar */
static void kb_glide_done(struct kb *kb)
{
    for (int i = 0; i < 2; i++) if (kb->trail[i]) lv_obj_add_flag(kb->trail[i], LV_OBJ_FLAG_HIDDEN);
    kb_voc_begin(kb);
    if (!kb->voc.w || kb->pn < 2) return;
    kb_layout_geo(kb);
    struct st_match best[ST_BEST];
    uint32_t t0 = sl_ms();
    int n = st_decode(&kb->geo, &kb->voc, kb->px, kb->py, kb->pn, best);
    sl_log("keyboard: glide of %d points -> %s (%d candidates, %lu ms)", kb->pn,
           n ? kb->voc.arena + kb->voc.w[best[0].word].off : "-", n, (unsigned long)(sl_ms() - t0));
    if (!n) return;
    const char *t = kb_text(kb);
    int b = kb_cursor_byte(kb);
    if (b && t[b - 1] != ' ' && t[b - 1] != '\n') kb_put(kb, " ");
    bool cap = kb->shift && (kb_def_now(kb)->flags & KBF_CASE);
    kb->nsug = 0;
    for (int i = 0; i < n && kb->nsug < 3; i++)
    {
        char *o = kb->sug[kb->nsug++];
        const char *w = kb->voc.arena + kb->voc.w[best[i].word].off;
        strlcpy(o, w, sizeof kb->sug[0]);
        if (cap) { const char *q = w; uint32_t c = st_utf8_next(&q); int l = st_utf8_put(o, st_upper(c)); strlcpy(o + l, q, sizeof kb->sug[0] - l); }
    }
    kb_put(kb, kb->sug[0]);
    if (kb->shift == 1) kb_set_shift(kb, 0);
    kb->sug_glide = true;
    kb_bar_show(kb);
    if (kb->on_text) kb->on_text(kb);
}
static void t9_pick(struct kb *kb, int i);
static void kb_sug_pick(struct kb *kb, int slot)
{
    static const int which[3] = { 1, 0, 2 };   /* bar slot -> suggestion */
    int i = which[slot];
    if (i >= kb->nsug) return;
    if (kb->sug_t9) { t9_pick(kb, i); return; }
    char w[sizeof kb->sug[0]];
    strlcpy(w, kb->sug[i], sizeof w);
    const char *t = kb_text(kb);
    int end = kb_cursor_byte(kb);
    if (kb->sug_letter)
    {
        kb_del(kb, 1);
        kb_put(kb, w);
    }
    else
    {
        int start = kb_word_start(kb, end);
        kb_del(kb, kb_cps(t, start, end));
        kb_put(kb, w);
        if (!kb->sug_glide && kb->kind != KB_SEARCH) kb_put(kb, " ");
    }
    sfx_play(SFX_TAP);
    kb->sug_glide = kb->sug_letter = false;
    kb_bar_clear(kb);
    kb_changed(kb);
}

/* ------------------------------------------------------------ pinyin */
/* Letters spell syllables; the bar offers what they can be: whole words
 * first (typed in full, or by their syllables' first letters: "zjl" for
 * 周杰伦), then the characters of the first syllable, the commonest first.
 * A pick types it and takes its letters off the front. */
static int py_cmp_n(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++)
    {
        if (!b[i]) return 1;
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
    }
    return 0;
}
/* the syllable that is exactly s[0..n), or -1 */
static int py_syl_exact(const char *s, int n)
{
    int lo = 0, hi = (int)ARRAYLEN(py_syl) - 1;
    while (lo <= hi)
    {
        int m = (lo + hi) / 2, c = py_cmp_n(s, py_syl[m][0], n);
        if (!c && !py_syl[m][0][n]) return m;
        if (c < 0 || (!c && py_syl[m][0][n])) hi = m - 1; else lo = m + 1;
    }
    return -1;
}
/* the first syllable that starts with s[0..n), or -1 */
static int py_syl_prefix(const char *s, int n)
{
    int lo = 0, hi = (int)ARRAYLEN(py_syl) - 1, at = -1;
    while (lo <= hi)
    {
        int m = (lo + hi) / 2, c = py_cmp_n(s, py_syl[m][0], n);
        if (c <= 0) { if (!c) at = m; hi = m - 1; } else lo = m + 1;
    }
    return at;
}
/* the first syllable's length: the longest whole one, or the letters that begin one */
static int py_first(const char *s)
{
    int n = (int)strlen(s);
    for (int k = n < 6 ? n : 6; k > 0; k--) if (py_syl_exact(s, k) >= 0) return k;
    for (int k = n < 6 ? n : 6; k > 0; k--) if (py_syl_prefix(s, k) >= 0) return -k;
    return 1;
}
/* do these letters spell syllables i.. of a word, each in full or begun?
 * 2: every one typed in full; 1: some only begun ("zjl"); 0: no */
static int py_wm(const char *b, const char *const *sy, const int *sl, int i, int ns)
{
    if (i == ns) return *b ? 0 : 2;
    int best = 0;
    for (int k = sl[i]; k >= 1 && best < 2; k--)
    {
        if (strncmp(b, sy[i], k)) continue;
        int r = py_wm(b + k, sy, sl, i + 1, ns);
        if (r && (k < sl[i] || r < 2)) r = 1;
        if (r > best) best = r;
    }
    return best;
}
static int py_word_match(const char *buf, const char *py)
{
    const char *sy[8];
    int sl[8], ns = 0;
    for (const char *p = py; *p && ns < 8;)
    {
        sy[ns] = p;
        while (*p && *p != ' ') p++;
        sl[ns] = (int)(p - sy[ns]);
        ns++;
        while (*p == ' ') p++;
    }
    return py_wm(buf, sy, sl, 0, ns);
}
static void kb_py_add(struct kb *kb, const char *hz, int len, int used)
{
    if (kb->py_n >= (int)ARRAYLEN(kb->py_c)) return;
    for (int i = 0; i < kb->py_n; i++) if (kb->py_clen[i] == len && !memcmp(kb->py_c[i], hz, len)) return;
    kb->py_c[kb->py_n] = hz; kb->py_clen[kb->py_n] = (uint8_t)len; kb->py_used[kb->py_n] = (uint8_t)used;
    kb->py_n++;
}
static void kb_py_candidates(struct kb *kb)
{
    const char *b = kb->py_buf;
    int n = (int)strlen(b);
    kb->py_n = 0;
    if (!n) return;
    if (kb_def_now(kb)->flags & KBF_KANA)
    {   /* the whole of it: hiragana, the kanji words it reads as, katakana */
        ja_kana(b, kb->ja_c[0], sizeof kb->ja_c[0], true);
        ja_katakana(kb->ja_c[0], kb->ja_c[1], sizeof kb->ja_c[1]);
        kb_py_add(kb, kb->ja_c[0], (int)strlen(kb->ja_c[0]), n);
        for (int w = 0; w < (int)ARRAYLEN(ja_words); w++)
            if (!strcmp(ja_words[w][0], kb->ja_c[0])) kb_py_add(kb, ja_words[w][1], (int)strlen(ja_words[w][1]), n);
        kb_py_add(kb, kb->ja_c[1], (int)strlen(kb->ja_c[1]), n);
        return;
    }
    /* whole words: in full, then by their syllables' first letters */
    for (int pass = 2; pass >= 1; pass--)
        for (int w = 0; w < (int)ARRAYLEN(py_words); w++)
            if (py_word_match(b, py_words[w][0]) == pass) kb_py_add(kb, py_words[w][1], (int)strlen(py_words[w][1]), n);
    /* words spelt in full by the letters' start ("yinyueh": 音乐, then the h stays) */
    for (int w = 0; w < (int)ARRAYLEN(py_words); w++)
    {
        char whole[32];
        int m = 0;
        for (const char *p = py_words[w][0]; *p && m < (int)sizeof whole - 1; p++) if (*p != ' ') whole[m++] = *p;
        if (m < n && !strncmp(b, whole, m)) kb_py_add(kb, py_words[w][1], (int)strlen(py_words[w][1]), m);
    }
    /* the first syllable's characters */
    int k = py_first(b);
    if (k > 0)
    {
        int s = py_syl_exact(b, k);
        for (const char *c = py_syl[s][1]; *c && kb->py_n < (int)ARRAYLEN(kb->py_c);)
        {
            const char *at = c;
            st_utf8_next(&c);
            kb_py_add(kb, at, (int)(c - at), k);
        }
    }
    else
    {
        /* begun: the commonest character of each syllable it can start, in turn */
        k = -k;
        int s0 = py_syl_prefix(b, k);
        for (int round = 0; round < 4; round++)
            for (int s = s0; s >= 0 && s < (int)ARRAYLEN(py_syl) && !py_cmp_n(b, py_syl[s][0], k); s++)
            {
                const char *c = py_syl[s][1];
                for (int r = 0; r < round && *c; r++) st_utf8_next(&c);
                if (!*c) continue;
                const char *at = c;
                st_utf8_next(&c);
                kb_py_add(kb, at, (int)(c - at), k);
            }
    }
}
static void kb_py_pick_cb(lv_event_t *e);
/* the letters as syllables ("ni'hao"), then the candidates, in a row that scrolls sideways */
static void kb_py_refresh(struct kb *kb)
{
    kb_py_candidates(kb);
    bool on = kb->py_buf[0] != 0;
    if (!kb->py_row && on)
    {
        kb->py_row = box(kb->root);
        lv_obj_set_size(kb->py_row, kb->w - 2 * KB_PAD_X, KB_BAR_H);
        lv_obj_set_pos(kb->py_row, KB_PAD_X, KB_TOP);
        lv_obj_set_flex_flow(kb->py_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(kb->py_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(kb->py_row, SL_SP_XS, 0);
        lv_obj_add_flag(kb->py_row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_USER_1);
        lv_obj_set_scroll_dir(kb->py_row, LV_DIR_HOR);
        lv_obj_set_scrollbar_mode(kb->py_row, LV_SCROLLBAR_MODE_OFF);
        lv_obj_remove_flag(kb->py_row, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    }
    if (!kb->py_row) return;
    lv_obj_update_flag(kb->py_row, LV_OBJ_FLAG_HIDDEN, !on);
    lv_obj_update_flag(kb->bar, LV_OBJ_FLAG_HIDDEN, on);
    lv_obj_clean(kb->py_row);
    if (!on) return;
    char shown[96], *o = shown;
    if (kb_def_now(kb)->flags & KBF_KANA) { ja_kana(kb->py_buf, shown, sizeof shown, false); o = shown + strlen(shown); }
    else for (const char *p = kb->py_buf; *p && o < shown + 61;)
    {
        int k = py_first(p);
        if (k < 0) k = -k;
        if (p != kb->py_buf) *o++ = '\'';
        memcpy(o, p, k); o += k; p += k;
    }
    *o = '\0';
    lv_obj_t *l = text(kb->py_row, "", sl_fonts[0][1], C_SUB);
    lv_label_set_text(l, shown);
    lv_obj_set_style_pad_hor(l, SL_SP_S, 0);
    for (int i = 0; i < kb->py_n; i++)
    {
        char hz[48];
        int len = kb->py_clen[i] < (int)sizeof hz - 1 ? kb->py_clen[i] : (int)sizeof hz - 1;
        memcpy(hz, kb->py_c[i], len);
        hz[len] = '\0';
        lv_obj_t *c = box(kb->py_row);
        lv_obj_set_size(c, LV_SIZE_CONTENT, KB_BAR_H - 4);
        lv_obj_set_style_pad_hor(c, SL_SP_S + SL_SP_XS, 0);
        lv_obj_set_style_radius(c, sl_zune() ? 0 : SL_R_SM, 0);
        lv_obj_set_style_bg_color(c, lv_color_hex(C_CARD_P), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_t *t = text(c, "", i ? &ss_regular_23 : sl_fonts[0][4], i ? C_TEXT : C_ACCENT_TXT);
        lv_label_set_text(t, hz);
        lv_obj_center(t);
        on_click(c, kb_py_pick_cb, kb);
        lv_obj_set_user_data(c, (void *)(intptr_t)i);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_EVENT_BUBBLE);
    }
}
static void kb_py_pick(struct kb *kb, int i)
{
    if (i < 0 || i >= kb->py_n) return;
    char hz[48];
    int len = kb->py_clen[i] < (int)sizeof hz - 1 ? kb->py_clen[i] : (int)sizeof hz - 1;
    memcpy(hz, kb->py_c[i], len);
    hz[len] = '\0';
    int used = kb->py_used[i];
    memmove(kb->py_buf, kb->py_buf + used, strlen(kb->py_buf + used) + 1);
    kb_put(kb, hz);
    sfx_play(SFX_TAP);
    kb_py_refresh(kb);
    kb_changed(kb);
}
/* what is being spelt goes in as it stands: the letters, or for romaji its kana */
static void kb_py_flush(struct kb *kb)
{
    if (!kb->py_buf[0]) return;
    if (kb_def_now(kb)->flags & KBF_KANA)
    {
        char k[sizeof kb->ja_c[0]];
        ja_kana(kb->py_buf, k, sizeof k, true);
        kb_put(kb, k);
    }
    else kb_put(kb, kb->py_buf);
    kb->py_buf[0] = '\0';
}
static void kb_py_pick_cb(lv_event_t *e)
{
    kb_py_pick(lv_event_get_user_data(e), (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e)));
}

/* ------------------------------------------------------------ the phone keypad */
/* Twelve keys as on a phone: 2 abc to 9 wxyz, 1 for punctuation, the
 * space bar for 0's space. Predictive (T9): each key once, and the
 * commonest word those keys spell is shown as it grows, the next ones in
 * the bar; the space keeps it. Multi-tap: a key pressed again within a
 * moment goes on to its next letter. Holding a key types its digit. The
 * words come from the other keyboard's dictionary when it is a Latin one
 * (German beside the keypad types German), else the menu language's. */
static const char *const t9_keys[10] = { " 0", ".,?!'-1", "abc", "def", "ghi", "jkl", "mno", "pqrs", "tuv", "wxyz" };
#define T9_TAP_MS 900           /* a second press within this goes on to the next letter */
static bool kb_t9(const struct kb *kb) { return (kb_def_now(kb)->flags & KBF_T9) && !kb->page; }
static bool t9_predictive(const struct kb *kb) { return global_settings.sloppy_kb_t9 != 0 && kb->kind != KB_PASSWORD; }
static const char *t9_dict(void)
{
    int other = kb_chosen(kb_chosen(0) == KB_T9 ? 1 : 0);
    if (other != KB_T9 && kb_defs[other].dict && (kb_defs[other].flags & KBF_CASE) && other != KB_RU)
        return kb_defs[other].dict;
    int l = kb_of_language(global_settings.sloppy_language);   /* a Latin one's words, else English */
    return kb_defs[l].dict && (kb_defs[l].flags & KBF_CASE) && l != KB_RU ? kb_defs[l].dict : kb_dict_en;
}
/* the key a letter is on, accents on their plain letter's; -1 for none */
static int t9_digit(uint32_t c)
{
    c = st_lower(c);
    uint32_t f = st_fold(c);
    if (f) c = f;
    return c >= 'a' && c <= 'z' ? "22233344455566677778889999"[c - 'a'] - '0' : -1;
}
/* a key's letters as multi-tap goes round them: with the dictionary's
 * accented ones after the plain ones, then the digit */
static int t9_cycle(int d, uint32_t *out, int max)
{
    int n = 0;
    for (const char *s = t9_keys[d]; *s && n < max; ) out[n++] = st_utf8_next(&s);
    if (d >= 2)
    {
        static const char *const more[10] = { NULL, NULL, "\xc3\xa0\xc3\xa1\xc3\xa2\xc3\xa4\xc3\xa7", "\xc3\xa8\xc3\xa9\xc3\xaa\xc3\xab",
            "\xc3\xac\xc3\xad\xc3\xae\xc3\xaf", NULL, "\xc3\xb1\xc3\xb2\xc3\xb3\xc3\xb4\xc3\xb6", "\xc3\x9f", "\xc3\xb9\xc3\xba\xc3\xbb\xc3\xbc", NULL };
        if (t9_dict() != kb_dict_en && more[d])
            for (const char *s = more[d]; *s && n < max; ) out[n++] = st_utf8_next(&s);
        if (n < max) out[n++] = '0' + d;
    }
    return n;
}
/* the dictionary's words those keys spell, the commonest first: whole
 * words of that length, then the beginnings of longer ones */
static void t9_find(struct kb *kb)
{
    int n = (int)strlen(kb->t9_seq);
    kb->t9_n = 0;
    for (int pass = 0; pass < 2 && kb->t9_n < 8; pass++)
        for (const char *p = t9_dict(); *p && kb->t9_n < 8; )
        {
            while (*p == ' ') p++;
            const char *w = p, *q = p;
            int len = 0, ok = 1;
            while (*q && *q != ' ')
            {
                const char *at = q;
                uint32_t c = st_utf8_next(&q);
                if (len < n && t9_digit(c) != kb->t9_seq[len] - '0') ok = 0;
                if (len == n) { if (pass == 1) { q = at; break; } }
                len++;
            }
            const char *end = q;
            while (*q && *q != ' ') q++;
            p = q;
            if (!ok || len < n || (pass == 0 && len != n) || end - w > ST_WORD_MAX * 3) continue;
            char cand[ST_WORD_MAX * 3 + 1];
            memcpy(cand, w, end - w);
            cand[end - w] = '\0';
            int dup = 0;
            for (int i = 0; i < kb->t9_n && !dup; i++) dup = !strcmp(kb->t9_cand[i], cand);
            if (!dup) strlcpy(kb->t9_cand[kb->t9_n++], cand, sizeof kb->t9_cand[0]);
        }
}
/* the word in the field for the keys pressed, the bar's choices beside it */
static void t9_show(struct kb *kb)
{
    char w[ST_WORD_MAX * 3 + 1];
    if (kb->t9_n) strlcpy(w, kb->t9_cand[kb->t9_sel % kb->t9_n], sizeof w);
    else
    {
        /* nothing in the dictionary: each key's first letter, to go on from */
        int k = 0;
        for (const char *d = kb->t9_seq; *d && k < (int)sizeof w - 4; d++) w[k++] = t9_keys[*d - '0'][0];
        w[k] = '\0';
    }
    if (kb->t9_cap && w[0]) { const char *q = w; uint32_t c = st_utf8_next(&q); char t[sizeof w]; int l = st_utf8_put(t, st_upper(c)); strlcpy(t + l, q, sizeof t - l); strlcpy(w, t, sizeof w); }
    kb_del(kb, kb->t9_shown);
    kb->t9_shown = kb_put(kb, w);        /* a full field takes only some: Delete must not go further */
    kb->nsug = 0;
    for (int i = 0; i < kb->t9_n && kb->nsug < 3; i++)
    {
        strlcpy(kb->sug[kb->nsug], kb->t9_cand[(kb->t9_sel + i) % kb->t9_n], sizeof kb->sug[0]);
        if (kb->t9_cap) { char *o = kb->sug[kb->nsug]; const char *q = o; uint32_t c = st_utf8_next(&q); char t[sizeof kb->sug[0]]; int l = st_utf8_put(t, st_upper(c)); strlcpy(t + l, q, sizeof t - l); strlcpy(o, t, sizeof kb->sug[0]); }
        kb->nsug++;
    }
    kb->sug_t9 = kb->nsug > 0;
    kb_bar_show(kb);
    if (kb->on_text) kb->on_text(kb);
}
/* the word as it stands is kept; the next key starts another */
static void t9_commit(struct kb *kb)
{
    kb->t9_seq[0] = '\0';
    kb->t9_shown = kb->t9_n = kb->t9_sel = 0;
    kb->t9_last = -1;
    if (kb->sug_t9) { kb->sug_t9 = false; kb_bar_clear(kb); }
}
static void t9_pick(struct kb *kb, int i)
{
    if (!kb->t9_n) return;
    kb->t9_sel = (kb->t9_sel + i) % kb->t9_n;
    t9_show(kb);
    sfx_play(SFX_TAP);
}
static bool t9_type(struct kb *kb, uint32_t c)
{
    char s[8];
    s[st_utf8_put(s, c)] = '\0';
    return kb_put(kb, s) > 0;
}
/* a key pressed: d is its digit */
static void t9_press(struct kb *kb, int d)
{
    uint32_t now = lv_tick_get();
    sfx_play(SFX_TAP);
    if (d >= 2 && t9_predictive(kb) && strlen(kb->t9_seq) < ST_WORD_MAX)
    {
        if (!kb->t9_seq[0]) kb->t9_cap = kb->shift != 0;
        size_t n = strlen(kb->t9_seq);
        kb->t9_seq[n] = (char)('0' + d);
        kb->t9_seq[n + 1] = '\0';
        kb->t9_sel = 0;
        t9_find(kb);
        t9_show(kb);
        if (kb->shift == 1) kb_set_shift(kb, 0);
        return;
    }
    if (kb->t9_seq[0]) t9_commit(kb);
    uint32_t cyc[24];
    int n = t9_cycle(d, cyc, 24);
    if (kb->t9_last == d && now - kb->t9_at < T9_TAP_MS)
    {
        kb_del(kb, 1);                   /* the letter before, for its next one */
        kb->t9_idx = (kb->t9_idx + 1) % n;
    }
    else
    {
        kb->t9_idx = 0;
        kb->t9_cap = kb->shift != 0;
    }
    kb->t9_at = now;
    /* a full field: the next press of the key is not "its next letter", or it takes the one before */
    kb->t9_last = t9_type(kb, kb->t9_cap && (kb_def_now(kb)->flags & KBF_CASE) ? st_upper(cyc[kb->t9_idx]) : cyc[kb->t9_idx]) ? d : -1;
    if (kb->shift == 1) kb_set_shift(kb, 0);
    kb->sug_glide = kb->sug_letter = false;
    kb_changed(kb);
}
/* held: the key's digit, as it is */
static void t9_hold(struct kb *kb, int d)
{
    t9_commit(kb);
    t9_type(kb, '0' + d);
    sfx_play(SFX_TAP);
    kb_changed(kb);
}
/* Delete, the keypad's way: in a word being spelt, its last key */
static bool t9_backspace(struct kb *kb)
{
    kb->t9_last = -1;
    size_t n = strlen(kb->t9_seq);
    if (!n) return false;
    kb->t9_seq[n - 1] = '\0';
    if (n == 1)
    {
        kb_del(kb, kb->t9_shown);
        t9_commit(kb);
        kb_changed(kb);
        return true;
    }
    kb->t9_sel = 0;
    t9_find(kb);
    t9_show(kb);
    return true;
}

/* ------------------------------------------------------------ building */
static void kb_build_keys(struct kb *kb);
static void kb_pop_hide(struct kb *kb)
{
    if (kb->pop) lv_obj_add_flag(kb->pop, LV_OBJ_FLAG_HIDDEN);
    if (kb->acc) lv_obj_add_flag(kb->acc, LV_OBJ_FLAG_HIDDEN);
}
static void kb_add_key(struct kb *kb, int act, uint32_t cp, int w, int row, const char *label, int icon)
{
    if (kb->nk >= KB_KEYS_MAX) return;
    struct kb_key *k = &kb->k[kb->nk];
    k->act = (uint8_t)act; k->cp = cp; k->row = (uint8_t)row; k->col = 0;
    k->o = kb_key_obj(kb, w, KB_KEY_H);
    k->l = NULL;
    uint32_t ink = kb->pal ? kb->pal_ink : act == KA_ENTER ? sl_accent_ink() : C_TEXT;
    if (icon)
    {
        k->l = sl_icon(k->o, icon, 26, ink, kb_key_bg(act));
        lv_obj_center(k->l);
    }
    else if (act == KA_CHAR)
    {
        char s[8];
        s[st_utf8_put(s, kb_case(kb, cp))] = '\0';
        k->l = lv_label_create(k->o);
        lv_label_set_text(k->l, s);
        /* the biggest of three that fits the key with room either side (ض's long tail, eleven across) */
        const lv_font_t *const sizes[] = { sl_fonts[0][5], sl_fonts[0][4], sl_fonts[0][3] };
        for (int f = 0; f < 3; f++)
        {
            lv_point_t z;
            lv_text_get_size(&z, lv_label_get_text(k->l), sizes[f], 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            lv_obj_set_style_text_font(k->l, sizes[f], 0);
            if (z.x <= w - 2 * SL_SP_S) break;
        }
        lv_obj_set_style_text_color(k->l, lv_color_hex(C_TEXT), 0);
        lv_obj_center(k->l);
    }
    else if (label)
    {
        k->l = text(k->o, label, act == KA_SPACE ? sl_fonts[0][1] : sl_fonts[0][2], act == KA_SPACE ? C_SUB : ink);
        if (act == KA_ENTER) lv_obj_set_style_text_color(k->l, lv_color_hex(ink), 0);
        lv_obj_update_layout(k->l);
        if (lv_obj_get_width(k->l) > w - 2 * SL_SP_S)
        {
            lv_obj_set_width(k->l, w - 2 * SL_SP_S);
            lv_label_set_long_mode(k->l, LV_LABEL_LONG_DOT);
        }
        lv_obj_center(k->l);
    }
    if ((act == KA_ENTER || act == KA_HIDE) && kb->enter) lv_obj_add_event_cb(k->o, kb->enter, LV_EVENT_CLICKED, NULL);
    kb->nk++;
    kb_key_paint(kb, kb->nk - 1, false);
}

/* the keypad: three columns of letters and one of keys that do things,
 * a key's letters large and its digit small in its corner */
static void t9_label(struct kb *kb, int i)
{
    struct kb_key *k = &kb->k[i];
    if (!k->l) return;
    const char *s = t9_keys[k->cp - '0'];
    char b[16];
    int n = 0;
    for (const char *q = s; *q && n < (int)sizeof b - 4; )
    {
        uint32_t c = st_utf8_next(&q);
        if (k->cp == '1' && c == '1') continue;
        n += st_utf8_put(b + n, kb->shift && k->cp >= '2' ? st_upper(c) : c);
    }
    b[n] = '\0';
    label_if_changed(k->l, b);
}
static void t9_mode_label(struct kb *kb)
{
    for (int i = 0; i < kb->nk; i++)
        if (kb->k[i].act == KA_T9MODE && kb->k[i].l)
            label_if_changed(kb->k[i].l, t9_predictive(kb) ? "T9" : "abc");
}
static void t9_build(struct kb *kb)
{
    int gw = kb->w - 2 * KB_PAD_X, u = (gw - 3 * KB_GAP) / 4;
    for (int r = 0; r < 3; r++)
    {
        int y = KB_TOP + KB_BAR_H + r * (KB_KEY_H + KB_ROW_GAP), first = kb->nk;
        for (int c = 0; c < 3; c++)
        {
            int d = r * 3 + c + 1;
            kb_add_key(kb, KA_T9, '0' + d, u, r, NULL, 0);
            struct kb_key *k = &kb->k[kb->nk - 1];
            k->col = (uint8_t)c;
            k->l = lv_label_create(k->o);
            lv_obj_set_style_text_font(k->l, sl_fonts[0][3], 0);
            lv_obj_set_style_text_color(k->l, lv_color_hex(kb->pal ? kb->pal_ink : C_TEXT), 0);
            lv_obj_center(k->l);
            t9_label(kb, kb->nk - 1);
            lv_obj_t *dg = lv_label_create(k->o);
            char ds[2] = { (char)('0' + d), '\0' };
            lv_label_set_text(dg, ds);
            lv_obj_set_style_text_font(dg, &ss_regular_16, 0);
            lv_obj_set_style_text_color(dg, lv_color_hex(kb->pal ? kb->pal_ink : C_SUB), 0);
            lv_obj_align(dg, LV_ALIGN_TOP_RIGHT, -SL_SP_S, SL_SP_XS);
        }
        if (r == 0) kb_add_key(kb, KA_BKSP, 0, u, r, LV_SYMBOL_BACKSPACE, 0);
        else if (r == 1) kb_add_key(kb, KA_SHIFT, 0, u, r, NULL, IC_SHIFT);
        else kb_add_key(kb, KA_T9MODE, 0, u, r, t9_predictive(kb) ? "T9" : "abc", 0);
        int x = KB_PAD_X;
        for (int i = first; i < kb->nk; i++) { lv_obj_set_pos(kb->k[i].o, x, y); x += u + KB_GAP; }
    }
    /* the bottom: the symbols, the globe, the space (0's place), Enter */
    int y = KB_TOP + KB_BAR_H + 3 * (KB_KEY_H + KB_ROW_GAP), first = kb->nk;
    bool globe = kb_two();
    int mode_w = globe ? (u - KB_GAP) / 2 : u;
    kb_add_key(kb, KA_MODE, 0, mode_w, 3, "123", 0);
    if (globe) kb_add_key(kb, KA_GLOBE, 0, u - KB_GAP - mode_w, 3, NULL, IC_GLOBE);
    kb_add_key(kb, KA_SPACE, ' ', 2 * u + KB_GAP, 3, kb_def_now(kb)->name, 0);
    if (kb->kind == KB_SEARCH) kb_add_key(kb, KA_HIDE, 0, u, 3, LV_SYMBOL_DOWN, 0);
    else kb_add_key(kb, KA_ENTER, 0, u, 3, kb->enter_label ? kb->enter_label : "Done", 0);
    int x = KB_PAD_X;
    for (int i = first; i < kb->nk; i++)
    {
        lv_obj_set_pos(kb->k[i].o, x, y);
        x += lv_obj_get_style_width(kb->k[i].o, 0) + KB_GAP;
    }
}

/* the rows as the language lays them out, centred; the widest row sets
 * the key width, Shift and Delete take what is left of their row */
static void kb_build_keys(struct kb *kb)
{
    const struct kb_def *d = kb_def_now(kb);
    lv_obj_clean(kb->keys);
    kb->nk = 0;
    kb->hot = -1;
    if (kb_t9(kb)) { t9_build(kb); kb_shift_icon(kb); return; }
    static const char *const sym[2][3] = {
        { "1234567890", "-/:;()$&@\"", NULL },
        { "[]{}#%^*+=", "_\\|~<>\xe2\x82\xac\xc2\xa3\xc2\xa5\xe2\x80\xa2", NULL } };
    const char *rows[3];
    if (kb->page) { rows[0] = sym[kb->page - 1][0]; rows[1] = sym[kb->page - 1][1]; rows[2] = d->punct; }
    else { rows[0] = d->rows[0]; rows[1] = d->rows[1]; rows[2] = d->rows[2]; }
    int count[3], cols = 0;
    for (int r = 0; r < 3; r++)
    {
        count[r] = 0;
        for (const char *s = rows[r]; *s; st_utf8_next(&s)) count[r]++;
        if (count[r] > cols) cols = count[r];
    }
    if (cols < 10) cols = 10;
    int gw = kb->w - 2 * KB_PAD_X, u = (gw - (cols - 1) * KB_GAP) / cols;
    bool shift_row = kb->page || (d->flags & KBF_CASE);
    bool rtl_letters = !kb->page && (d->flags & KBF_RTL);
    for (int r = 0; r < 3; r++)
    {
        int y = KB_TOP + KB_BAR_H + r * (KB_KEY_H + KB_ROW_GAP);
        int first = kb->nk;
        /* Delete: the third row's end, or the first row's when the letters
         * leave no room there (Hebrew); Shift (or the symbols' page key) starts the third */
        bool del_here = rtl_letters ? r == 0 && count[0] < cols - 1 : r == 2;
        if (rtl_letters && r == 2 && !(count[0] < cols - 1)) del_here = true;
        int letters_w = count[r] * u + (count[r] - 1) * KB_GAP;
        int spare = gw - letters_w - 2 * KB_GAP;
        int side = u * 3 / 2;
        if (r == 2 && shift_row && del_here && spare / 2 < side) side = spare / 2;
        if (r != 2 && del_here && spare < side) side = spare;
        if (r == 2 && shift_row)
            kb_add_key(kb, kb->page ? KA_SYMS : KA_SHIFT, 0, side, r, kb->page == 1 ? "#+=" : kb->page == 2 ? "123" : NULL,
                       kb->page ? 0 : IC_SHIFT);
        const char *s = rows[r];
        uint32_t c;
        int col = 0;
        while ((c = st_utf8_next(&s)))
        {
            kb_add_key(kb, KA_CHAR, c, u, r, NULL, 0);
            kb->k[kb->nk - 1].col = (uint8_t)col++;
        }
        if (del_here) kb_add_key(kb, KA_BKSP, 0, side, r, LV_SYMBOL_BACKSPACE, 0);
        /* lay the row out, centred */
        int total = 0;
        for (int i = first; i < kb->nk; i++) total += lv_obj_get_style_width(kb->k[i].o, 0) + (i > first ? KB_GAP : 0);
        int x = KB_PAD_X + (gw - total) / 2;
        for (int i = first; i < kb->nk; i++)
        {
            lv_obj_set_pos(kb->k[i].o, x, y);
            x += lv_obj_get_style_width(kb->k[i].o, 0) + KB_GAP;
        }
    }
    /* the bottom row */
    int y = KB_TOP + KB_BAR_H + 3 * (KB_KEY_H + KB_ROW_GAP);
    int first = kb->nk;
    int mode_w = u * 3 / 2, small = u + u / 4, enter_w = u * 2 + KB_GAP;
    kb_add_key(kb, KA_MODE, 0, mode_w, 3, kb->page ? d->abc : "123", 0);
    if (kb_two()) kb_add_key(kb, KA_GLOBE, 0, small, 3, NULL, IC_GLOBE);
    if (kb->on_draw) kb_add_key(kb, KA_DRAW, 0, small, 3, LV_SYMBOL_EDIT, 0);
    int used = 0;
    for (int i = first; i < kb->nk; i++) used += lv_obj_get_style_width(kb->k[i].o, 0) + KB_GAP;
    bool dot = kb->kind != KB_SEARCH;
    int right = (dot ? small + KB_GAP : 0) + enter_w;
    int space_w = gw - used - right - KB_GAP;
    kb_add_key(kb, KA_SPACE, ' ', space_w, 3, d->name, 0);
    if (dot) kb_add_key(kb, KA_CHAR, (d->flags & KBF_COMPOSE) ? 0x3002 : '.', small, 3, NULL, 0);
    if (kb->kind == KB_SEARCH) kb_add_key(kb, KA_HIDE, 0, enter_w, 3, LV_SYMBOL_DOWN, 0);
    else kb_add_key(kb, KA_ENTER, 0, enter_w, 3, kb->enter_label ? kb->enter_label : "Done", 0);
    int x = KB_PAD_X;
    for (int i = first; i < kb->nk; i++)
    {
        lv_obj_set_pos(kb->k[i].o, x, y);
        x += lv_obj_get_style_width(kb->k[i].o, 0) + KB_GAP;
    }
    kb_shift_icon(kb);
}

/* ------------------------------------------------------------ touch */
static int kb_key_at(struct kb *kb, int x, int y)
{
    int best = -1;
    int32_t best_d = INT32_MAX;
    for (int i = 0; i < kb->nk; i++)
    {
        lv_area_t a;
        lv_obj_get_coords(kb->k[i].o, &a);
        int32_t dx = x < a.x1 ? a.x1 - x : x > a.x2 ? x - a.x2 : 0;
        int32_t dy = y < a.y1 ? a.y1 - y : y > a.y2 ? y - a.y2 : 0;
        if (!dx && !dy) return i;
        int32_t d = dx * dx + dy * dy;
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}
/* the letter, big, over the key under the finger */
static void kb_pop_show(struct kb *kb, int i)
{
    struct kb_key *k = &kb->k[i];
    if (k->act != KA_CHAR) { kb_pop_hide(kb); return; }
    lv_area_t a, r;
    lv_obj_get_coords(k->o, &a);
    lv_obj_get_coords(kb->root, &r);
    int w = lv_area_get_width(&a) + 2 * SL_SP_S, h = 64;
    int x = (a.x1 + a.x2) / 2 - r.x1 - w / 2;
    if (x < 2) x = 2;
    if (x > kb->w - w - 2) x = kb->w - w - 2;
    lv_obj_set_size(kb->pop, w, h);
    lv_obj_set_pos(kb->pop, x, a.y1 - r.y1 - h - SL_SP_XS);
    char s[8];
    s[st_utf8_put(s, kb_case(kb, k->cp))] = '\0';
    label_if_changed(lv_obj_get_child(kb->pop, 0), s);
    lv_obj_remove_flag(kb->pop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(kb->pop);
}
static void kb_hot(struct kb *kb, int i)
{
    if (i == kb->hot) return;
    if (kb->hot >= 0 && kb->hot < kb->nk) kb_key_paint(kb, kb->hot, false);
    kb->hot = i;
    if (i < 0) { kb_pop_hide(kb); return; }
    kb_key_paint(kb, i, true);
    kb_pop_show(kb, i);
}
static void kb_trail(struct kb *kb)
{
    lv_area_t r;
    lv_obj_get_coords(kb->root, &r);
    int from = kb->pn > 64 ? kb->pn - 64 : 0, n = 0;
    for (int i = from; i < kb->pn; i++)
        kb->trail_pts[n++] = (lv_point_precise_t){ (lv_value_precise_t)(kb->px[i] - r.x1), (lv_value_precise_t)(kb->py[i] - r.y1) };
    for (int i = 0; i < 2; i++)
    {
        lv_obj_set_style_line_color(kb->trail[i], lv_color_hex(C_ACCENT), 0);
        lv_line_set_points(kb->trail[i], kb->trail_pts, n);
        lv_obj_remove_flag(kb->trail[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(kb->trail[i]);
    }
}

static void kb_acc_track(struct kb *kb, int x);
/* the accents a held letter offers, in a row over it; the finger slides to one */
static void kb_acc_open(struct kb *kb)
{
    if (kb->hot < 0) return;
    struct kb_key *k = &kb->k[kb->hot];
    if (k->act != KA_CHAR) return;
    kb->acc_n = 0;
    kb->acc_cp[kb->acc_n++] = kb_case(kb, k->cp);   /* the letter itself first: held and let go, it types */
    if (!kb->page && k->row == 0 && k->col < 10 && (kb_def_now(kb)->flags & (KBF_CASE | KBF_COMPOSE)))
        kb->acc_cp[kb->acc_n++] = k->col == 9 ? '0' : '1' + k->col;
    const char *a = kb_accents(k->cp);
    for (const char *s = a; s && *s && kb->acc_n < 12;)
    {
        uint32_t c = st_utf8_next(&s);
        kb->acc_cp[kb->acc_n++] = c == '\'' ? 0 : kb_case(kb, c);
    }
    if (kb->acc_n < 2) return;
    kb_pop_hide(kb);
    lv_obj_clean(kb->acc);
    int cw = 44, n = kb->acc_n;
    lv_area_t ka, r;
    lv_obj_get_coords(k->o, &ka);
    lv_obj_get_coords(kb->root, &r);
    int w = n * cw + 2 * SL_SP_XS, x = (ka.x1 + ka.x2) / 2 - r.x1 - cw / 2 - SL_SP_XS;
    if (x + w > kb->w - 2) x = kb->w - 2 - w;
    if (x < 2) x = 2;
    lv_obj_set_size(kb->acc, w, 64);
    lv_obj_set_pos(kb->acc, x, ka.y1 - r.y1 - 64 - SL_SP_XS);
    for (int i = 0; i < n; i++)
    {
        lv_obj_t *c = box(kb->acc);
        lv_obj_set_size(c, cw, 56);
        lv_obj_set_pos(c, SL_SP_XS + i * cw, SL_SP_XS);
        lv_obj_set_style_radius(c, sl_zune() ? 0 : SL_R_SM, 0);
        lv_obj_set_style_bg_color(c, lv_color_hex(C_ACCENT), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
        char s[12];
        if (kb->acc_cp[i]) s[st_utf8_put(s, kb->acc_cp[i])] = '\0';
        else strlcpy(s, "\xd7\x92'", sizeof s);   /* ג' */
        lv_obj_t *l = lv_label_create(c);
        lv_label_set_text(l, s);
        lv_obj_set_style_text_font(l, sl_fonts[0][5], 0);
        lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), 0);
        lv_obj_center(l);
    }
    kb->acc_sel = -1;
    kb->accenting = true;
    lv_obj_remove_flag(kb->acc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(kb->acc);
    lv_obj_update_layout(kb->acc);
    lv_area_t aa;
    lv_obj_get_coords(kb->acc, &aa);
    kb_acc_track(kb, aa.x1);           /* the letter itself, chosen until the finger moves */
    kb->acc_x0 = (int)kb->px[kb->pn - 1];
}
static void kb_acc_track(struct kb *kb, int x)
{
    lv_area_t a;
    lv_obj_get_coords(kb->acc, &a);
    int i = (x - a.x1 - SL_SP_XS) / 44;
    if (i < 0) i = 0;
    if (i >= kb->acc_n) i = kb->acc_n - 1;
    if (i == kb->acc_sel) return;
    for (int j = 0; j < kb->acc_n; j++)
    {
        lv_obj_t *c = lv_obj_get_child(kb->acc, j);
        lv_obj_set_style_bg_opa(c, j == i ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(c, 0), lv_color_hex(j == i ? sl_accent_ink() : C_TEXT), 0);
    }
    kb->acc_sel = i;
}

static void kb_type_cp(struct kb *kb, uint32_t c)
{
    char s[8];
    s[st_utf8_put(s, c)] = '\0';
    if ((kb_def_now(kb)->flags & KBF_KANA) && c == 0x30fc && kb->py_buf[0]) c = '-';   /* the long mark, inside the word */
    if ((kb_def_now(kb)->flags & KBF_COMPOSE) && (c == '-' ? (kb_def_now(kb)->flags & KBF_KANA) && kb->py_buf[0] : !kb->page && c < 0x80 && c >= 'a' && c <= 'z'))
    {
        size_t n = strlen(kb->py_buf);
        if (n + 1 < sizeof kb->py_buf) { kb->py_buf[n] = (char)c; kb->py_buf[n + 1] = '\0'; }
        sfx_play(SFX_TAP);
        kb_py_refresh(kb);
        return;
    }
    kb_put(kb, s);
    sfx_play(SFX_TAP);
    if (kb->shift == 1) kb_set_shift(kb, 0);
    kb->sug_glide = kb->sug_letter = false;
    kb_changed(kb);
}
static void kb_backspace(struct kb *kb, bool word)
{
    if (kb_t9(kb) && !word && t9_backspace(kb)) return;
    if (kb->t9_seq[0]) t9_commit(kb);   /* whole words: the word spelt goes as any other */
    if (kb->py_buf[0])
    {
        kb->py_buf[strlen(kb->py_buf) - 1] = '\0';
        kb_py_refresh(kb);
        return;
    }
    const char *t = kb_text(kb);
    int end = kb_cursor_byte(kb);
    if (!end) return;
    int n = 1;
    if (word)
    {
        int s = end;
        while (s > 0 && t[s - 1] == ' ') s--;
        s = kb_word_start(kb, s);
        n = kb_cps(t, s, end);
    }
    kb_del(kb, n);
    kb->sug_glide = kb->sug_letter = false;
    kb_changed(kb);
}
static void kb_space(struct kb *kb)
{
    t9_commit(kb);                       /* the keypad's word stays as it is */
    if (kb->py_buf[0])                   /* the first candidate */
    {
        if (kb->py_n) kb_py_pick(kb, 0);
        else { kb_py_flush(kb); kb_py_refresh(kb); kb_changed(kb); }
        return;
    }
    kb_put(kb, " ");
    sfx_play(SFX_TAP);
    kb->sug_glide = kb->sug_letter = false;
    kb_changed(kb);
}
static void kb_switch(struct kb *kb, int lay)
{
    t9_commit(kb);
    kb->lay = lay;
    kb->page = 0;
    kb->py_buf[0] = '\0';
    kb->shift = 0;
    kb_bar_clear(kb);
    kb_build_keys(kb);
    lv_obj_update_layout(kb->keys);
    kb_autocap(kb);
    kb_voc_begin(kb);
    kb_py_refresh(kb);
    if (kb->timer) lv_timer_resume(kb->timer);
}
static void kb_press_key(struct kb *kb, int i)
{
    struct kb_key *k = &kb->k[i];
    switch (k->act)
    {
    case KA_CHAR: kb_type_cp(kb, kb_case(kb, k->cp)); break;
    case KA_T9: t9_press(kb, (int)(k->cp - '0')); break;
    case KA_T9MODE:
        t9_commit(kb);
        global_settings.sloppy_kb_t9 = !global_settings.sloppy_kb_t9;
        t9_mode_label(kb);
        sfx_play(SFX_TAP);
        settings_save();
        break;
    case KA_SPACE: kb_space(kb); break;
    case KA_BKSP: if (!kb->deleted) { kb_backspace(kb, false); sfx_play(SFX_UI_BACK); } break;
    case KA_SHIFT:
    {
        /* a quick second tap locks capitals */
        static uint32_t last;
        uint32_t now = lv_tick_get();
        int s = kb->shift == 1 && now - last < 400 ? 2 : kb->shift ? 0 : 1;
        last = now;
        kb_set_shift(kb, s);
        break;
    }
    case KA_MODE:
        t9_commit(kb);
        kb->page = kb->page ? 0 : 1;
        kb->shift = 0;
        kb_bar_clear(kb);
        kb_build_keys(kb);
        if (!kb->page) kb_autocap(kb);
        break;
    case KA_SYMS: kb->page = kb->page == 1 ? 2 : 1; kb_build_keys(kb); break;
    case KA_GLOBE:
        kb_py_flush(kb);
        kb_switch(kb, kb->lay == kb_chosen(0) ? kb_chosen(1) : kb_chosen(0));
        sfx_play(SFX_TAP);
        break;
    case KA_ENTER:
        t9_commit(kb);
        if (kb->py_buf[0]) { kb_py_flush(kb); kb_py_refresh(kb); kb_changed(kb); break; }
        /* last: it may close the page, and this keyboard with it */
        if (kb->enter) lv_obj_send_event(k->o, LV_EVENT_CLICKED, NULL);
        break;
    case KA_DRAW: if (kb->on_draw) kb->on_draw(kb); break;
    case KA_HIDE: if (kb->enter) lv_obj_send_event(k->o, LV_EVENT_CLICKED, NULL); break;
    }
}

static void kb_tick(lv_timer_t *t)
{
    struct kb *kb = lv_timer_get_user_data(t);
    bool busy = false;
    if (kb->down && kb->hot >= 0 && kb->hot < kb->nk && kb->k[kb->hot].act == KA_BKSP)
    {
        busy = true;
        uint32_t now = lv_tick_get();
        if (now - kb->held_at >= 450 && (int32_t)(now - kb->next_at) >= 0)
        {
            /* letters at first, whole words once it has run a while */
            bool word = kb->deleted >= 8;
            kb_backspace(kb, word);
            kb->deleted++;
            kb->next_at = now + (word ? 260 : 75);
        }
    }
    if (kb_voc_building(kb)) { kb_voc_step(kb); busy = true; }
    if (!busy) lv_timer_pause(t);
}

static void kb_keys_cb(lv_event_t *e)
{
    struct kb *kb = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    lv_point_t p = { 0, 0 };
    if (in) lv_indev_get_point(in, &p);
    if (code == LV_EVENT_PRESSED)
    {
            kb_live = kb;
        kb->pn = 0;
        kb->gliding = kb->cursoring = kb->accenting = false;
        kb->down = true;
        kb->deleted = 0;
        kb->px[kb->pn] = p.x; kb->py[kb->pn++] = p.y;
        kb_hot(kb, kb_key_at(kb, p.x, p.y));
        kb->cur_x = p.x;
        kb->held_at = lv_tick_get();
        kb->next_at = kb->held_at + 450;
        if (kb->hot >= 0 && kb->k[kb->hot].act == KA_BKSP)
        {
            kb_backspace(kb, false);         /* at once; the timer repeats it while held */
            sfx_play(SFX_UI_BACK);
            kb->deleted = 1;
            lv_timer_resume(kb->timer);
        }
        if (kb_glides(kb)) kb_voc_begin(kb);
    }
    else if (code == LV_EVENT_PRESSING)
    {
        if (!kb->down) return;
        if (kb->accenting)
        {
            if (abs(p.x - kb->acc_x0) > 10) { kb->acc_x0 = -1000; kb_acc_track(kb, p.x); }
            return;
        }
        float lx = kb->px[kb->pn - 1], ly = kb->py[kb->pn - 1];
        if ((p.x - lx) * (p.x - lx) + (p.y - ly) * (p.y - ly) < 9) return;   /* still */
        if (kb->pn < KB_PATH_MAX) { kb->px[kb->pn] = p.x; kb->py[kb->pn++] = p.y; }
        int hot_act = kb->hot >= 0 ? kb->k[kb->hot].act : -1;
        if (hot_act == KA_SPACE && !kb->py_buf[0] && (kb->cursoring || abs(p.x - (int)kb->px[0]) > 16))
        {
            if (!kb->cursoring) t9_commit(kb);   /* the cursor leaves the word being spelt */
            /* along the space bar: the cursor follows, a letter per 14 px */
            kb->cursoring = true;
            int steps = (p.x - kb->cur_x) / 14;
            bool back = (steps < 0) != ((kb_def_now(kb)->flags & KBF_RTL) != 0);
            for (int s = abs(steps); s > 0 && kb->ta; s--)
                back ? lv_textarea_cursor_left(kb->ta) : lv_textarea_cursor_right(kb->ta);
            kb->cur_x += steps * 14;
            if (steps) { kb->sug_glide = false; kb_suggest(kb); }
            return;
        }
        if (!kb->gliding && hot_act == KA_CHAR && kb->k[kb->hot].row < 3 && kb_glides(kb) && st_is_swipe(kb->px, kb->py, kb->pn, kb->geo.n ? kb->geo.key_w : 44))
        {
            kb->gliding = true;
            kb_hot(kb, -1);
        }
        if (kb->gliding) kb_trail(kb);
        else if (hot_act != KA_BKSP) kb_hot(kb, kb_key_at(kb, p.x, p.y));
    }
    else if (code == LV_EVENT_LONG_PRESSED)
    {
        if (!kb->gliding && !kb->cursoring && kb->hot >= 0 && kb->k[kb->hot].act == KA_CHAR) kb_acc_open(kb);
        else if (!kb->cursoring && kb->hot >= 0 && kb->k[kb->hot].act == KA_T9)
        {
            t9_hold(kb, (int)(kb->k[kb->hot].cp - '0'));   /* held: its digit, and no letter on release */
            kb->t9_held = true;
        }
    }
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
    {
        if (!kb->down) return;
        kb->down = false;
        int hot = kb->hot;
        bool lost = code == LV_EVENT_PRESS_LOST;
        if (kb->accenting)
        {
            if (!lost && kb->acc_sel >= 0)
            {
                uint32_t c = kb->acc_cp[kb->acc_sel];
                if (c) kb_type_cp(kb, c);
                else { kb_put(kb, "\xd7\x92'"); sfx_play(SFX_TAP); kb_changed(kb); }
            }
        }
        else if (kb->gliding) { if (!lost) kb_glide_done(kb); }
        bool tap = !kb->accenting && !kb->gliding && !kb->cursoring && !lost && hot >= 0 && !kb->t9_held;
        kb->t9_held = false;
        kb->gliding = kb->cursoring = kb->accenting = false;
        for (int i = 0; i < 2; i++) if (kb->trail[i]) lv_obj_add_flag(kb->trail[i], LV_OBJ_FLAG_HIDDEN);
        kb_pop_hide(kb);
        if (kb->hot >= 0 && kb->hot < kb->nk) kb_key_paint(kb, kb->hot, false);
        kb->hot = -1;
        if (tap) kb_press_key(kb, hot);  /* last: Enter may close the page and this keyboard */
    }
}

static void kb_sug_cb(lv_event_t *e)
{
    struct kb *kb = lv_event_get_user_data(e);
    lv_obj_t *o = lv_event_get_current_target(e);
    for (int i = 0; i < 3; i++) if (kb->sug_o[i] == o) kb_sug_pick(kb, i);
}

static void kb_ta_cb(lv_event_t *e)
{
    struct kb *kb = lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_DELETE) { if (lv_event_get_target(e) == kb->ta) kb->ta = NULL; return; }
}
static void kb_changed(struct kb *kb)
{
    kb_autocap(kb);
    kb_suggest(kb);
    if (kb->on_text) kb->on_text(kb);
}

static void kb_deleted(lv_event_t *e)
{
    struct kb *kb = lv_event_get_user_data(e);
    if (lv_event_get_target(e) != kb->root) return;
    if (kb->timer) lv_timer_delete(kb->timer);
    if (kb->ta) lv_obj_remove_event_cb_with_user_data(kb->ta, kb_ta_cb, kb);
    kb_voc_free(kb);
    if (kb_live == kb) kb_live = NULL;
    free(kb);
}

/* The keyboard on page p, typing into ta: kind is KB_TEXT, KB_NAME
 * (capitals start it), KB_PASSWORD (no suggestions or glides) or
 * KB_SEARCH (a hide key where Enter is); enter runs on the wide key at the
 * bottom right, which says enter_label. */
static struct kb *kb_attach(lv_obj_t *p, lv_obj_t *ta, int kind, lv_event_cb_t enter, const char *enter_label)
{
    uint32_t t0 = sl_ms();
    struct kb *kb = calloc(1, sizeof *kb);
    if (!kb) return NULL;
    kb->ta = ta;
    kb->kind = kind;
    kb->enter = enter;
    kb->enter_label = enter_label;
    kb->voc_lay = -1;
    kb->voc_more = -1;
    kb->hot = -1;
    kb->t9_last = -1;
    kb->lay = kb_chosen(0);
    lv_obj_update_layout(p);
    kb->w = lv_obj_get_content_width(p);
    if (kb->w <= 0 || kb->w > W) kb->w = W;
    kb->root = box(p);
    lv_obj_set_size(kb->root, kb->w, KB_H);
    lv_obj_align(kb->root, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(kb->root, lv_color_hex(sl_theme() == 1 ? C_CARD_P : C_BG), 0);
    lv_obj_set_style_bg_opa(kb->root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(kb->root, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(kb->root, sl_theme() == 1 ? 0 : 1, 0);
    lv_obj_set_style_border_color(kb->root, lv_color_hex(C_CARD_P), 0);
    lv_obj_add_flag(kb->root, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_1);   /* typing is not a back-swipe */
    lv_obj_add_flag(kb->root, LV_OBJ_FLAG_OVERFLOW_VISIBLE);   /* a top-row key's bubble rises over the page */
    lv_obj_remove_flag(kb->root, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(kb->root, kb_deleted, LV_EVENT_DELETE, kb);
    /* the bar: three words, hairlines between */
    kb->bar = box(kb->root);
    lv_obj_set_size(kb->bar, kb->w - 2 * KB_PAD_X, KB_BAR_H);
    lv_obj_set_pos(kb->bar, KB_PAD_X, KB_TOP);
    int cw = (kb->w - 2 * KB_PAD_X) / 3;
    for (int i = 0; i < 3; i++)
    {
        lv_obj_t *c = box(kb->bar);
        lv_obj_set_size(c, cw, KB_BAR_H);
        lv_obj_set_pos(c, i * cw, 0);
        lv_obj_set_style_radius(c, sl_zune() ? 0 : SL_R_SM, 0);
        lv_obj_set_style_bg_color(c, lv_color_hex(C_CARD_P), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_t *l = text(c, "", i == 1 ? sl_fonts[0][3] : &ss_regular_21, i == 1 ? C_TEXT : C_SUB);
        lv_obj_set_width(l, cw - 2 * SL_SP_S);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        label_lines(l, 1);
        lv_obj_center(l);
        on_click(c, kb_sug_cb, kb);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_EVENT_BUBBLE);
        kb->sug_o[i] = c;
        if (i)
        {
            lv_obj_t *h = box(kb->bar);
            lv_obj_set_size(h, 1, 24);
            lv_obj_set_pos(h, i * cw, (KB_BAR_H - 24) / 2);
            lv_obj_set_style_bg_color(h, lv_color_hex(C_SUB), 0);
            lv_obj_set_style_bg_opa(h, 60, 0);
            lv_obj_add_flag(h, LV_OBJ_FLAG_HIDDEN);   /* only between words */
            kb->sep[i - 1] = h;
        }
    }
    /* the keys: one touch surface */
    kb->keys = box(kb->root);
    lv_obj_set_size(kb->keys, kb->w, KB_H);
    lv_obj_set_pos(kb->keys, 0, 0);
    lv_obj_add_flag(kb->keys, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK | LV_OBJ_FLAG_USER_1);
    lv_obj_remove_flag(kb->keys, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(kb->keys, kb_keys_cb, LV_EVENT_ALL, kb);
    lv_obj_move_background(kb->keys);
    lv_obj_move_foreground(kb->bar);
    /* the bubble over a pressed key, the accents' row, the glide's trail */
    kb->pop = box(kb->root);
    lv_obj_set_style_radius(kb->pop, sl_zune() ? 0 : SL_R_MD, 0);
    lv_obj_set_style_bg_color(kb->pop, lv_color_hex(sl_theme() == 1 ? C_CARD : C_CARD_P), 0);
    sl_elevate(kb->pop, SL_E1, 0);                 /* a hairline where black meets black */
    lv_obj_set_style_bg_opa(kb->pop, LV_OPA_COVER, 0);
    sl_elevate(kb->pop, SL_E3, 0);
    lv_obj_t *pl = lv_label_create(kb->pop);
    lv_label_set_text(pl, "");
    lv_obj_set_style_text_font(pl, &ss_semibold_30, 0);
    lv_obj_set_style_text_color(pl, lv_color_hex(C_TEXT), 0);
    lv_obj_center(pl);
    lv_obj_add_flag(kb->pop, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(kb->pop, LV_OBJ_FLAG_CLICKABLE);
    kb->acc = box(kb->root);
    lv_obj_set_style_radius(kb->acc, sl_zune() ? 0 : SL_R_MD, 0);
    lv_obj_set_style_bg_color(kb->acc, lv_color_hex(sl_theme() == 1 ? C_CARD : C_CARD_P), 0);
    sl_elevate(kb->acc, SL_E1, 0);                 /* a hairline where black meets black */
    lv_obj_set_style_bg_opa(kb->acc, LV_OPA_COVER, 0);
    sl_elevate(kb->acc, SL_E3, 0);
    lv_obj_add_flag(kb->acc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(kb->acc, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < 2; i++)
    {
        kb->trail[i] = lv_line_create(kb->root);
        lv_obj_set_style_line_width(kb->trail[i], i ? 6 : 16, 0);   /* a soft glow under a firm line */
        lv_obj_set_style_line_rounded(kb->trail[i], true, 0);
        lv_obj_set_style_line_opa(kb->trail[i], i ? LV_OPA_90 : 70, 0);
        lv_obj_remove_flag(kb->trail[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(kb->trail[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (ta) lv_obj_add_event_cb(ta, kb_ta_cb, LV_EVENT_DELETE, kb);
    kb->timer = lv_timer_create(kb_tick, 40, kb);
    lv_timer_pause(kb->timer);
    kb_build_keys(kb);
    kb_autocap(kb);
    kb_live = kb;
    kb_built_us = (sl_ms() - t0) * 1000;
    return kb;
}
/* the keyboard in a page's own colours (the pet's LCD greens) */
static void kb_colours(struct kb *kb, uint32_t tray, uint32_t key, uint32_t pressed, uint32_t ink, int radius)
{
    if (!kb) return;
    kb->pal = true;
    kb->pal_tray = tray; kb->pal_key = key; kb->pal_press = pressed; kb->pal_ink = ink; kb->pal_radius = radius;
    lv_obj_set_style_bg_color(kb->root, lv_color_hex(tray), 0);
    lv_obj_set_style_border_width(kb->root, 0, 0);
    for (int i = 0; i < 3; i++) lv_obj_set_style_text_color(lv_obj_get_child(kb->sug_o[i], 0), lv_color_hex(ink), 0);
    kb_build_keys(kb);
}
/* type into another field from now on */
static void kb_retarget(struct kb *kb, lv_obj_t *ta)
{
    if (!kb || kb->ta == ta) return;
    if (kb->ta) lv_obj_remove_event_cb_with_user_data(kb->ta, kb_ta_cb, kb);
    kb->ta = ta;
    if (ta) lv_obj_add_event_cb(ta, kb_ta_cb, LV_EVENT_DELETE, kb);
    t9_commit(kb);
    kb_bar_clear(kb);
    kb_autocap(kb);
}
/* numbers to type (an EQ band's values): the symbols' first page */
static void kb_digits(struct kb *kb)
{
    if (!kb) return;
    kb->page = 1;
    kb->shift = 0;
    kb_build_keys(kb);
}
/* the keyboard on page p, if it has one */
static struct kb *kb_of_page(lv_obj_t *p)
{
    for (uint32_t i = 0; p && i < lv_obj_get_child_count(p); i++)
    {
        lv_obj_t *c = lv_obj_get_child(p, i);
        for (uint32_t k = 0; k < lv_obj_get_event_count(c); k++)
        {
            lv_event_dsc_t *d = lv_obj_get_event_dsc(c, k);
            if (lv_event_dsc_get_cb(d) == kb_deleted) return lv_event_dsc_get_user_data(d);
        }
    }
    return NULL;
}
