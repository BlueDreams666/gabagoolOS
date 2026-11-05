/* Letters for other scripts: Cyrillic, Chinese, Japanese, Korean and Thai,
 * and emoji (Noto Color Emoji's pictures, Noto Emoji's outlines where a
 * picture is missing, in the text's colour). GPLv2+.
 * Included by sloppy.c before anything is drawn.
 *
 * The compiled fonts carry Latin, Hebrew and Arabic. Every one of them
 * ends its fallback chain at one of the fonts below (ss_arabic_<size>
 * falls back to sl_xf_<size>), which hold no letters of their own: they
 * read a pack of them from the card (.rockbox/sloppy/fonts/<pack>.slf, made by
 * sloppy-dev/slfpack.swift) the first time a word needs one, and keep it.
 * A player that never shows a Russian, Chinese, Korean or Thai word never
 * spends the memory: 0.56, 0.81 and 1.14 MB for Chinese and kana at 16, 20
 * and 24 px (4,277 characters), 0.23 to 0.46 MB more at a size when a word in
 * traditional characters or Japanese kanji turns up; 0.28, 0.41 and 0.54 MB
 * for Korean (KS X 1001's 2,350 syllables and the jamo), 1.0 to 2.0 MB more
 * only for a syllable outside them; 6 to 17 KB a size for Cyrillic, 7 to 17 KB
 * for Thai. Each proxy then falls back to our symbols (sl_sym_<px>, which pass anything they do not draw to Montserrat), as the
 * Arabic fonts did.
 *
 * Letters drawn one at a time still come out as a shaper would put them:
 * a Thai mark is the pack's glyph for where it stands (Core Text's own
 * substitutions and positions, made by sloppy-dev/kb/gen-thai.py), and the
 * jamo and kana a Mac writes apart in file names (한 as three jamo, ガ as
 * カ and a sound mark) are drawn as the one letter they make. */
#define XF_SIZES 7
static const uint8_t xf_px[XF_SIZES] = { 16, 18, 21, 23, 26, 28, 30 };   /* the Arabic fonts' sizes */
struct xf_pack {
    const char *name;
    bool tried;                 /* looked for once; a missing file is not looked for again */
    uint8_t *mem;
    uint32_t n;
    const uint8_t *cp, *dsc, *bm;
};
enum { XF_CYR = 0, XF_CJK = XF_SIZES, XF_CJKT = XF_CJK + 3, XF_KOR = XF_CJKT + 3, XF_KORX = XF_KOR + 3, XF_TH = XF_KORX + 3,
       XF_EMO = XF_TH + XF_SIZES, XF_PACKS = XF_EMO + 3 };
static struct xf_pack xf_packs[XF_PACKS] = {
    { .name = "cyr16" }, { .name = "cyr18" }, { .name = "cyr21" }, { .name = "cyr23" }, { .name = "cyr26" }, { .name = "cyr28" }, { .name = "cyr30" },
    { .name = "cjk16" }, { .name = "cjk20" }, { .name = "cjk24" },                /* simplified, kana, punctuation */
    { .name = "cjkt16" }, { .name = "cjkt20" }, { .name = "cjkt24" },             /* traditional forms of the common characters, Japanese kanji */
    { .name = "kor16" }, { .name = "kor20" }, { .name = "kor24" },                /* Hangul: KS X 1001's syllables, the jamo */
    { .name = "korx16" }, { .name = "korx20" }, { .name = "korx24" },             /* the other modern syllables */
    { .name = "th16" }, { .name = "th18" }, { .name = "th21" }, { .name = "th23" }, { .name = "th26" }, { .name = "th28" }, { .name = "th30" },
    { .name = "emo16" }, { .name = "emo20" }, { .name = "emo24" },             /* emoji: U+1Fxxx kept as xxx, the rest as themselves */
};
static unsigned long xf_bytes;          /* loaded, for the memory lines */

static bool xf_load(struct xf_pack *p)
{
    if (p->mem) return true;
    if (p->tried) return false;
    p->tried = true;
    char path[MAX_PATH];
    snprintf(path, sizeof path, ROCKBOX_DIR "/sloppy/fonts/%s.slf", p->name);
    uint32_t t0 = sl_ms();
    int fd = open(path, O_RDONLY);
    if (fd < 0) { sl_log("font: %s missing", path); return false; }
    uint8_t h[20];
    uint32_t n = 0, bytes = 0;
    if (read(fd, h, sizeof h) == (ssize_t)sizeof h && !memcmp(h, "SLF1", 4))
    {
        memcpy(&n, h + 12, 4);
        memcpy(&bytes, h + 16, 4);
    }
    size_t total = (size_t)n * 14 + bytes;
    uint8_t *m = n && n < 20000 && bytes < (8u << 20) ? malloc(total + 1) : NULL;
    if (!m || read(fd, m, total) != (ssize_t)total)
    {
        close(fd);
        free(m);
        sl_log("font: %s unreadable", path);
        return false;
    }
    close(fd);
    /* every letter's bitmap inside the pack: a damaged card's pack was
     * drawn from past its end */
    for (uint32_t g = 0; g < n; g++)
    {
        const uint8_t *e = m + (size_t)n * 2 + (size_t)g * 12;
        uint32_t off;
        memcpy(&off, e, 4);
        if (off > bytes || ((uint32_t)e[6] * e[7] + 1) / 2 > bytes - off)
        {
            free(m);
            sl_log("font: %s damaged (letter %u)", path, (unsigned)g);
            return false;
        }
    }
    p->mem = m;
    p->n = n;
    p->cp = m;
    p->dsc = m + (size_t)n * 2;
    p->bm = m + (size_t)n * 14;
    xf_bytes += total;
    sl_log("font: %s, %u letters, %lu KB in %lu ms (%lu KB of packs loaded)", p->name, (unsigned)n,
           (unsigned long)(total / 1024), (unsigned long)(sl_ms() - t0), xf_bytes / 1024);
    return true;
}
static int xf_find(const struct xf_pack *p, uint32_t c)
{
    int lo = 0, hi = (int)p->n - 1;
    while (lo <= hi)
    {
        int m = (lo + hi) / 2;
        uint16_t v;
        memcpy(&v, p->cp + m * 2, 2);
        if (v == c) return m;
        if (v < c) lo = m + 1; else hi = m - 1;
    }
    return -1;
}
static bool xf_cjk(uint32_t c)
{
    return (c >= 0x2e80 && c <= 0x9fff) || (c >= 0xf900 && c <= 0xfaff) || (c >= 0xff00 && c <= 0xffef);
}
/* LVGL asks for a letter's width with the letter after it, then draws it
 * asking with none: the next letter is kept from the one to the other */
static uint32_t xf_la_c, xf_la_next;
/* Thai: the letter its marks sit on, and an upper vowel already over it */
static uint32_t xf_th_base, xf_th_up;
/* a mark's glyph for where it stands: the keys sloppy-dev/kb/gen-thai.py
 * gives Core Text's shapes (keep the two in step) */
static uint32_t xf_thai_key(uint32_t c, uint32_t next)
{
    uint32_t b = xf_th_base;
    int tall = b == 0xe1b || b == 0xe1d || b == 0xe1f || b == 0xe2c, ctx = 0;   /* po pla, fo fa, fo fan, lo chula */
    int di = b == 0xe0e ? 0 : b == 0xe0f ? 1 : b == 0xe0d ? 2 : b == 0xe10 ? 3 : -1;
    bool upper = c == 0xe31 || (c >= 0xe34 && c <= 0xe37) || c == 0xe47 || c == 0xe4d || c == 0xe4e;
    if (c >= 0xe48 && c <= 0xe4c)                               /* a tone: over a vowel, before sara am, or alone */
    {
        int vi = xf_th_up == 0xe31 ? 0 : xf_th_up >= 0xe34 && xf_th_up <= 0xe37 ? (int)(xf_th_up - 0xe33) : -1;
        ctx = vi >= 0 ? 2 + vi * 2 + tall : next == 0xe33 ? 12 + tall : tall;
    }
    else if (upper || c == 0xe33)
    {
        ctx = tall;
        if (upper) xf_th_up = c;
    }
    else if (c >= 0xe38 && c <= 0xe3a) ctx = di >= 0 ? 14 + di : 0;
    else
    {
        xf_th_base = c;
        xf_th_up = 0;
        di = c == 0xe0e ? 0 : c == 0xe0f ? 1 : c == 0xe0d ? 2 : c == 0xe10 ? 3 : -1;
        if (di >= 0 && next >= 0xe38 && next <= 0xe3a) ctx = 18 + di;   /* yo ying and tho than lose their foot over a lower vowel */
    }
    return ctx ? 0xe000u | (uint32_t)ctx << 7 | (c - 0xe00) : c;
}
/* Hangul written apart (a Mac's file names): the jamo before a syllable's
 * last take no room, the last draws the syllable; one on its own is drawn
 * as the jamo a keyboard types */
static uint32_t xf_ko_l, xf_ko_v;
static const uint8_t xf_ko_cl[19] = { 1, 2, 4, 7, 8, 9, 17, 18, 19, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30 };
static const uint8_t xf_ko_ct[27] = { 1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 20, 21, 22, 23, 24, 26, 27, 28, 29, 30 };
static uint32_t xf_jamo(uint32_t c, uint32_t next)
{
    bool nv = next >= 0x1161 && next <= 0x1175, nt = next >= 0x11a8 && next <= 0x11c2;
    if (c <= 0x1112)
    {
        xf_ko_l = c;
        xf_ko_v = 0;
        return nv ? 0 : 0x3130u + xf_ko_cl[c - 0x1100];
    }
    if (c >= 0x1161 && c <= 0x1175)
    {
        if (!xf_ko_l) return 0x314fu + (c - 0x1161);
        xf_ko_v = c;
        return nt ? 0 : 0xac00u + ((xf_ko_l - 0x1100) * 21 + (c - 0x1161)) * 28;
    }
    if (xf_ko_l && xf_ko_v) return 0xac00u + ((xf_ko_l - 0x1100) * 21 + (xf_ko_v - 0x1161)) * 28 + (c - 0x11a7);
    return 0x3130u + xf_ko_ct[c - 0x11a8];
}
/* kana and a combining sound mark (カ and U+3099, as a Mac writes ガ): the one letter */
static uint32_t xf_kana_prev;
static uint32_t xf_kana_join(uint32_t c, uint32_t m)
{
    uint32_t k = c >= 0x30a1 && c <= 0x30f6 ? c - 0x60 : c;       /* katakana as their hiragana */
    bool ha = k == 0x306f || k == 0x3072 || k == 0x3075 || k == 0x3078 || k == 0x307b;
    if (m == 0x309a) return ha ? c + 2 : 0;
    if (m != 0x3099) return 0;
    if ((k >= 0x304b && k <= 0x3061 && (k & 1)) || k == 0x3064 || k == 0x3066 || k == 0x3068 || ha || c == 0x309d || c == 0x30fd) return c + 1;
    if (k == 0x3046) return c + 0x4e;                                 /* u: vu */
    if (c >= 0x30ef && c <= 0x30f2) return c + 8;                    /* wa wi we wo: va vi ve vo */
    return 0;
}
/* Colour emoji: Noto Color Emoji's pictures (emoc<px>.slf, made by
 * sloppy-dev/emojipack.swift), read from the card one at a time as a word
 * needs them and kept in a few slots, the least used given up first. Only
 * the slots are ever in memory (55 KB, taken the first time and only above
 * the memory floor), never a pack. A picture the pack lacks, or no room,
 * and the outlines above draw it in the text's colour. */
#define XE_KEY 8                        /* code points in a sequence, without U+200D and U+FE0F */
#define XE_ENT 36                       /* a pack's index entry */
#define XE_SLOTS 32
#define XE_MAX 24                       /* the biggest picture */
struct xe_slot {
    uint32_t key[XE_KEY];
    uint8_t len, cls, w, h, adv;
    int8_t below;
    bool none;                          /* not in the pack: the outlines draw it */
    uint32_t used;
    lv_image_dsc_t img;
    uint8_t px[XE_MAX * XE_MAX * 3];    /* RGB565, then alpha */
};
static struct xe_slot *xe_slots;
static uint32_t xe_clock;
static struct xe_pack { const char *name; uint16_t n, px; uint32_t index_off, data_off, retry; } xe_packs[3] = {
    { .name = "emoc16" }, { .name = "emoc20" }, { .name = "emoc24" },
};
static long sl_mem_available_kb(void);
static long sl_mem_floor_kb(void);
static int xe_cmp(const uint8_t *e, const uint32_t *key, int n)
{
    for (int i = 0; i < e[0] && i < n; i++)
    {
        uint32_t c = e[12 + i * 3] | e[13 + i * 3] << 8 | (uint32_t)e[14 + i * 3] << 16;
        if (c != key[i]) return c < key[i] ? -1 : 1;
    }
    return e[0] - n;
}
/* the picture for key in the pack for size class cls, or NULL */
static struct xe_slot *xe_get(int cls, const uint32_t *key, int n)
{
    if (!xe_slots)
    {
        long kb = sl_mem_available_kb();
        if (kb >= 0 && kb - (long)(XE_SLOTS * sizeof *xe_slots / 1024) < sl_mem_floor_kb()) return NULL;
        if (!(xe_slots = calloc(XE_SLOTS, sizeof *xe_slots))) return NULL;
    }
    struct xe_slot *s = xe_slots;
    for (int i = 0; i < XE_SLOTS; i++)
    {
        struct xe_slot *t = &xe_slots[i];
        if (t->len == n && t->cls == cls && !memcmp(t->key, key, (size_t)n * 4))
        {
            t->used = ++xe_clock;
            return t->none ? NULL : t;
        }
        if (t->used < s->used) s = t;
    }
    struct xe_pack *p = &xe_packs[cls];
    if (p->retry && (int32_t)(sl_ms() - p->retry) < 0) return NULL;   /* the card was away: not before then */
    char path[MAX_PATH];
    snprintf(path, sizeof path, ROCKBOX_DIR "/sloppy/fonts/%s.slf", p->name);
    int fd = open(path, O_RDONLY);
    if (fd < 0) { p->retry = sl_ms() + 5000; return NULL; }
    p->retry = 0;
    uint8_t e[XE_ENT];
    if (!p->n)
    {
        if (read(fd, e, 16) != 16 || memcmp(e, "SLE1", 4)) { close(fd); p->retry = sl_ms() + 60000; return NULL; }
        memcpy(&p->px, e + 4, 2);
        memcpy(&p->n, e + 6, 2);
        memcpy(&p->index_off, e + 8, 4);
        memcpy(&p->data_off, e + 12, 4);
    }
    int lo = 0, hi = p->n - 1, at = -1;
    while (lo <= hi)
    {
        int m = (lo + hi) / 2;
        if (lseek(fd, p->index_off + (off_t)m * XE_ENT, SEEK_SET) < 0 || read(fd, e, XE_ENT) != XE_ENT) break;
        int r = xe_cmp(e, key, n);
        if (!r) { at = m; break; }
        if (r < 0) lo = m + 1; else hi = m - 1;
    }
    if (s->len && !s->none) lv_image_cache_drop(&s->img);
    memset(s, 0, offsetof(struct xe_slot, px));
    memcpy(s->key, key, (size_t)n * 4);
    s->len = (uint8_t)n;
    s->cls = (uint8_t)cls;
    s->used = ++xe_clock;
    s->none = true;
    uint32_t off;
    memcpy(&off, e + 8, 4);
    size_t size = (size_t)e[1] * e[2] * 3;
    if (at >= 0 && e[1] && e[1] <= XE_MAX && e[2] <= XE_MAX &&
        lseek(fd, p->data_off + (off_t)off, SEEK_SET) >= 0 && read(fd, s->px, size) == (ssize_t)size)
    {
        s->none = false;
        s->w = e[1];
        s->h = e[2];
        s->adv = e[3];
        s->below = (int8_t)e[4];
        s->img.header.magic = LV_IMAGE_HEADER_MAGIC;
        s->img.header.cf = LV_COLOR_FORMAT_RGB565A8;
        s->img.header.w = s->w;
        s->img.header.h = s->h;
        s->img.header.stride = s->w * 2;
        s->img.data = s->px;
        s->img.data_size = size;
    }
    close(fd);
    return s->none ? NULL : s;
}
/* the slots go back with the other rebuildable memory (sl_mem_release) */
static void xe_free(void)
{
    if (!xe_slots) return;
    for (int i = 0; i < XE_SLOTS; i++)
        if (xe_slots[i].len && !xe_slots[i].none) lv_image_cache_drop(&xe_slots[i].img);
    free(xe_slots);
    xe_slots = NULL;
}
/* Sequences: 👍🏽, 🧛‍♂️, 🇺🇸, ❤️‍🔥 are one picture. Their parts take no
 * room and the last draws the picture, or its first emoji when the pack has
 * none for the whole: never a loose ♂, skin tone or flag letter. A right to
 * left line keeps a sequence in its order (lv_bidi.c's rtl_reverse). */
static bool xe_tone(uint32_t c) { return c >= 0x1f3fb && c <= 0x1f3ff; }
static bool xe_ri(uint32_t c) { return c >= 0x1f1e6 && c <= 0x1f1ff; }
static bool xe_symbol(uint32_t c)        /* the emoji among the symbols */
{
    return c == 0x203c || c == 0x2049 || c == 0x2122 || c == 0x2139 || c == 0x24c2 ||
           (c >= 0x2194 && c <= 0x21aa) || (c >= 0x231a && c <= 0x23ff) || (c >= 0x25aa && c <= 0x27bf) ||
           c == 0x2934 || c == 0x2935 || (c >= 0x2b05 && c <= 0x2b55);
}
static bool xe_pic(uint32_t c) { return (c >= 0x1f000 && c <= 0x1faff && !xe_tone(c)) || xe_symbol(c); }
static bool xe_part(uint32_t c)          /* joins a picture, never drawn itself */
{
    return c == 0x200d || c == 0x20e3 || c == 0xfe0e || c == 0xfe0f || xe_tone(c) || (c >= 0xe0020 && c <= 0xe007f);
}
static uint32_t xe_seq[XE_KEY];
static int xe_n;
static bool xe_wait;                    /* a picture next joins (after U+200D, or after a flag's first letter) */
static uint32_t xe_end_c;               /* a line's last letter, measured: its drawing is the same */
static uint32_t xe_last_c;
static lv_font_glyph_dsc_t xe_last;
static bool xe_last_r;
static bool xe_put(lv_font_glyph_dsc_t *d, const struct xe_slot *s)
{
    d->adv_w = s->adv;
    d->box_w = s->w;
    d->box_h = s->h;
    d->ofs_x = (s->adv - s->w) / 2;
    d->ofs_y = s->below;
    d->format = LV_FONT_GLYPH_FORMAT_IMAGE;
    d->is_placeholder = 0;
    d->gid.src = &s->img;
    return true;
}
static bool xf_glyph_one(const lv_font_t *f, lv_font_glyph_dsc_t *d, uint32_t c, uint32_t next, bool seq);
static bool xf_glyph_dsc(const lv_font_t *f, lv_font_glyph_dsc_t *d, uint32_t c, uint32_t next)
{
    bool again = false;
    if (next) { xf_la_c = c; xf_la_next = next; xe_end_c = 0; }
    else if (c == xf_la_c) { next = xf_la_next; xf_la_c = 0; again = true; }
    else { xf_la_c = 0; again = c == xe_end_c; xe_end_c = again ? 0 : c; }
    if (!xe_pic(c) && !xe_part(c)) { xe_n = 0; xe_wait = false; return xf_glyph_one(f, d, c, next, false); }
    if (again && c == xe_last_c) { *d = xe_last; return xe_last_r; }   /* drawn as it was measured */
    int si = (int)(intptr_t)f->user_data, cls = xf_px[si] <= 18 ? 0 : xf_px[si] <= 23 ? 1 : 2;
    if (c == 0x200d) xe_wait = xe_n > 0;
    else if (xe_tone(c))
    {
        if (xe_n && xe_n < XE_KEY) xe_seq[xe_n++] = c;
    }
    else if (xe_pic(c))
    {
        bool pair = xe_ri(c) && xe_n == 1 && xe_ri(xe_seq[0]) && xe_wait;
        if ((xe_wait || pair) && xe_n && xe_n < XE_KEY) xe_seq[xe_n++] = c;
        else { xe_seq[0] = c; xe_n = 1; }
        xe_wait = xe_ri(c) && xe_n == 1;   /* a flag's first letter waits for its second */
    }
    if (next == 0x200d && xe_n) xe_wait = true;   /* LVGL never asks for U+200D (lv_text_is_marker): seen from before it */
    bool more = xe_n && (xe_part(next) || (xe_wait && (xe_ri(xe_seq[0]) && xe_n == 1 ? xe_ri(next) : xe_pic(next))));
    bool r = true;
    if (!xe_n || more)
    {
        d->adv_w = 0;
        d->box_w = d->box_h = 0;
        d->ofs_x = d->ofs_y = 0;
        d->format = LV_FONT_GLYPH_FORMAT_A4;
        d->is_placeholder = 0;
        d->gid.index = 0;
    }
    else
    {
        uint32_t k[XE_KEY];
        int n = xe_n;
        memcpy(k, xe_seq, sizeof k);
        xe_n = 0;
        xe_wait = false;
        bool plain = n == 1 && xe_symbol(k[0]) && c == k[0];   /* ★ ✓ ▶ alone: as they always were */
        const struct xe_slot *s = NULL;
        lv_font_glyph_dsc_t t;
        if (plain && f->fallback && lv_font_get_glyph_dsc(f->fallback, &t, k[0], 0) && !t.is_placeholder) r = false;
        else if ((s = xe_get(cls, k, n)) || (n > 1 && (s = xe_get(cls, k, 1)))) xe_put(d, s);
        else r = xf_glyph_one(f, d, k[0], 0, true);   /* its outline */
    }
    xe_last_c = c;
    xe_last = *d;
    xe_last_r = r;
    return r;
}
/* one letter, or the first emoji of a sequence in its outline */
static bool xf_glyph_one(const lv_font_t *f, lv_font_glyph_dsc_t *d, uint32_t c, uint32_t next, bool seq)
{
    int si = (int)(intptr_t)f->user_data, pk[3], np = 0;
    int cls = xf_px[si] <= 18 ? 0 : xf_px[si] <= 23 ? 1 : 2;     /* hanzi and hangul fill their square: a size smaller sits level with Latin */
    uint32_t key = c, prev_kana = xf_kana_prev;
    bool hangul_jamo = c >= 0x1100 && c <= 0x11ff;
    if (c != 0x3099 && c != 0x309a) xf_kana_prev = 0;           /* a sound mark is asked for twice (its width, then drawn): both see the kana */
    if (!hangul_jamo) xf_ko_l = 0;
    if (c < 0xe00 || c > 0xe7f) xf_th_base = xf_th_up = 0;
    /* what joins emoji takes no room: the shaper above draws the picture */
    if (c == 0x200d || c == 0x20e3 || c == 0xfe0e || c == 0xfe0f ||
        (c >= 0x1f3fb && c <= 0x1f3ff) || (c >= 0xe0020 && c <= 0xe007f)) goto blank;
    if (c >= 0x1f000 && c <= 0x1faff)
    {
        key = c - 0x1f000;
        pk[np++] = XF_EMO + cls;
    }
    else if (xe_symbol(c))
    {
        /* ★ ✓ ▶ and the like: the symbols after us draw them as they always
         * have, the emoji only what nothing else has */
        lv_font_glyph_dsc_t t;
        if (!seq && f->fallback && lv_font_get_glyph_dsc(f->fallback, &t, c, 0) && !t.is_placeholder) return false;
        pk[np++] = XF_EMO + cls;
    }
    else if (c >= 0x400 && c <= 0x4ff) pk[np++] = XF_CYR + si;
    else if (c >= 0xe00 && c <= 0xe7f)
    {
        key = xf_thai_key(c, next);
        pk[np++] = XF_TH + si;
    }
    else if (hangul_jamo)
    {
        if (!(c <= 0x1112 || (c >= 0x1161 && c <= 0x1175) || (c >= 0x11a8 && c <= 0x11c2))) return false;   /* old Hangul: none */
        if (!(c = key = xf_jamo(c, next)))
            goto blank;
        pk[np++] = XF_KOR + cls;
        if (c >= 0xac00) pk[np++] = XF_KORX + cls;
    }
    else if ((c >= 0xac00 && c <= 0xd7a3) || (c >= 0x3130 && c <= 0x318f))
    {
        pk[np++] = XF_KOR + cls;
        if (c >= 0xac00) pk[np++] = XF_KORX + cls;
    }
    else if (xf_cjk(c))
    {
        if (c == 0x3099 || c == 0x309a)                                  /* a sound mark: in the kana before it, or on its own */
        {
            if (prev_kana && xf_kana_join(prev_kana, c)) goto blank;
            key = c + 2;
        }
        else if (c >= 0x3041 && c <= 0x30ff)
        {
            uint32_t j = xf_kana_join(c, next);
            if (j) key = j;
            xf_kana_prev = c;
        }
        pk[np++] = XF_CJK + cls;
        pk[np++] = XF_CJKT + cls;
        pk[np++] = XF_EMO + cls;                                      /* 〰 〽 ㊗ ㊙ */
    }
    for (int i = 0; i < np; i++)
    {
        struct xf_pack *p = &xf_packs[pk[i]];
        if (!xf_load(p)) continue;
        int g = xf_find(p, key);
        if (g < 0 && key != c) g = xf_find(p, c);                       /* a context the pack has no shape for: the letter itself */
        if (g < 0) continue;
        const uint8_t *e = p->dsc + (size_t)g * 12;
        uint16_t adv;
        memcpy(&adv, e + 4, 2);
        d->adv_w = (uint16_t)((adv + 8) >> 4);
        d->box_w = e[6];
        d->box_h = e[7];
        d->ofs_x = (int8_t)e[8];
        d->ofs_y = (int8_t)e[9];
        d->format = LV_FONT_GLYPH_FORMAT_A4;
        d->is_placeholder = 0;
        d->gid.index = (uint32_t)pk[i] << 16 | (uint32_t)g;
        return true;
    }
    return false;
blank:                                                                   /* drawn by the letter it joins */
    d->adv_w = 0;
    d->box_w = d->box_h = 0;
    d->ofs_x = d->ofs_y = 0;
    d->format = LV_FONT_GLYPH_FORMAT_A4;
    d->is_placeholder = 0;
    d->gid.index = 0;
    return true;
}
static const void *xf_glyph_bitmap(lv_font_glyph_dsc_t *d, lv_draw_buf_t *buf)
{
    static const uint8_t opa4[16] = { 0, 17, 34, 51, 68, 85, 102, 119, 136, 153, 170, 187, 204, 221, 238, 255 };
    if (d->format == LV_FONT_GLYPH_FORMAT_IMAGE) return d->gid.src;   /* a colour emoji */
    const struct xf_pack *p = &xf_packs[d->gid.index >> 16];
    if (!p->mem || !buf) return NULL;
    const uint8_t *e = p->dsc + (size_t)(d->gid.index & 0xffff) * 12;
    uint32_t off;
    memcpy(&off, e, 4);
    int w = e[6], h = e[7];
    if (!w || !h) return NULL;
    const uint8_t *in = p->bm + off;
    uint8_t *out = buf->data;
    uint32_t stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_A8);
    int i = 0;
    for (int y = 0; y < h; y++, out += stride)
        for (int x = 0; x < w; x++, i++)
            out[x] = opa4[i & 1 ? in[i >> 1] & 15 : in[i >> 1] >> 4];
    return buf;
}
/* The proxies: Montserrat's height and baseline (rows are laid out exactly
 * as before), the packs' letters, Montserrat's symbols after them */
#define XF_FONT(px_, i_, mont_, lh_, bl_) \
    const lv_font_t sl_xf_##px_ = { .get_glyph_dsc = xf_glyph_dsc, .get_glyph_bitmap = xf_glyph_bitmap, \
        .line_height = lh_, .base_line = bl_, .subpx = LV_FONT_SUBPX_NONE, \
        .underline_position = -2, .underline_thickness = 1, .dsc = NULL, \
        .fallback = &sl_sym_##mont_, .user_data = (void *)(intptr_t)(i_) };
XF_FONT(16, 0, 16, 18, 3)
XF_FONT(18, 1, 18, 21, 4)
XF_FONT(21, 2, 20, 22, 4)
XF_FONT(23, 3, 22, 24, 4)
XF_FONT(26, 4, 24, 27, 5)
XF_FONT(28, 5, 28, 30, 5)
XF_FONT(30, 6, 28, 30, 5)
