/* Letters for other scripts: Cyrillic, Chinese, Japanese, Korean and Thai,
 * and emoji (Noto Emoji's outlines, in the text's colour). GPLv2+.
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
static bool xf_glyph_dsc(const lv_font_t *f, lv_font_glyph_dsc_t *d, uint32_t c, uint32_t next)
{
    if (next) { xf_la_c = c; xf_la_next = next; }
    else if (c == xf_la_c) { next = xf_la_next; xf_la_c = 0; }
    else xf_la_c = 0;
    int si = (int)(intptr_t)f->user_data, pk[3], np = 0;
    int cls = xf_px[si] <= 18 ? 0 : xf_px[si] <= 23 ? 1 : 2;     /* hanzi and hangul fill their square: a size smaller sits level with Latin */
    uint32_t key = c, prev_kana = xf_kana_prev;
    bool hangul_jamo = c >= 0x1100 && c <= 0x11ff;
    if (c != 0x3099 && c != 0x309a) xf_kana_prev = 0;           /* a sound mark is asked for twice (its width, then drawn): both see the kana */
    if (!hangul_jamo) xf_ko_l = 0;
    if (c < 0xe00 || c > 0xe7f) xf_th_base = xf_th_up = 0;
    /* what joins emoji (a Bluetooth name's 🎧️, 👍🏽, 👩‍💻) takes no room: the
     * parts are drawn one by one, a skin tone not at all */
    if (c == 0x200d || c == 0x20e3 || c == 0xfe0e || c == 0xfe0f ||
        (c >= 0x1f3fb && c <= 0x1f3ff) || (c >= 0xe0020 && c <= 0xe007f)) goto blank;
    if (c >= 0x1f000 && c <= 0x1faff)
    {
        key = c - 0x1f000;
        pk[np++] = XF_EMO + cls;
    }
    else if (c == 0x203c || c == 0x2049 || c == 0x2122 || c == 0x2139 || c == 0x24c2 ||
             (c >= 0x2194 && c <= 0x21aa) || (c >= 0x231a && c <= 0x23ff) || (c >= 0x25aa && c <= 0x27bf) ||
             c == 0x2934 || c == 0x2935 || (c >= 0x2b05 && c <= 0x2b55))   /* the emoji among the symbols */
    {
        /* ★ ✓ ▶ and the like: the symbols after us draw them as they always
         * have, the emoji only what nothing else has */
        lv_font_glyph_dsc_t t;
        if (f->fallback && lv_font_get_glyph_dsc(f->fallback, &t, c, 0) && !t.is_placeholder) return false;
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
