/* Interface translations for Settings > Language. Each row is the English
 * text as written in the code, then Spanish, French, German, Hebrew and
 * Arabic. Simplified Chinese (language 6) and Italian, Dutch, Portuguese
 * (Brazil), Polish, Czech, Russian and Japanese (7 to 13) have a table each,
 * keyed by the same English (sloppy-dev/lang/gen-zh.py, gen-langs.py).
 * A missing translation (NULL) falls back to English. */
struct sl_phrase { const char *en, *t[5]; };
static struct sl_phrase sl_phrases[] = {
#include "sloppy_lang_table.h"
};
struct sl_phrase_one { const char *en, *tr; };
static struct sl_phrase_one sl_phrases_zh[] = {
#include "sloppy_lang_zh.h"
}, sl_phrases_it[] = {
#include "sloppy_lang_it.h"
}, sl_phrases_nl[] = {
#include "sloppy_lang_nl.h"
}, sl_phrases_pt[] = {
#include "sloppy_lang_pt.h"
}, sl_phrases_pl[] = {
#include "sloppy_lang_pl.h"
}, sl_phrases_cs[] = {
#include "sloppy_lang_cs.h"
}, sl_phrases_ru[] = {
#include "sloppy_lang_ru.h"
}, sl_phrases_ja[] = {
#include "sloppy_lang_ja.h"
};
/* 0 English, 1 Spanish, 2 French, 3 German, 4 Hebrew, 5 Arabic, 6 Chinese,
 * 7 Italian, 8 Dutch, 9 Portuguese (Brazil), 10 Polish, 11 Czech,
 * 12 Russian, 13 Japanese: global_settings.sloppy_language */
enum { SL_LANG_ZH = 6, SL_LANG_IT, SL_LANG_NL, SL_LANG_PT, SL_LANG_PL, SL_LANG_CS, SL_LANG_RU, SL_LANG_JA };
#define SL_LANG_LAST SL_LANG_JA
static struct sl_lang_one { struct sl_phrase_one *p; size_t n; bool sorted; } sl_one[] = {
    { sl_phrases_zh, ARRAYLEN(sl_phrases_zh), false }, { sl_phrases_it, ARRAYLEN(sl_phrases_it), false },
    { sl_phrases_nl, ARRAYLEN(sl_phrases_nl), false }, { sl_phrases_pt, ARRAYLEN(sl_phrases_pt), false },
    { sl_phrases_pl, ARRAYLEN(sl_phrases_pl), false }, { sl_phrases_cs, ARRAYLEN(sl_phrases_cs), false },
    { sl_phrases_ru, ARRAYLEN(sl_phrases_ru), false }, { sl_phrases_ja, ARRAYLEN(sl_phrases_ja), false },
};
/* a language's ISO code: the phone remote's page and the update notes */
static const char *sl_lang_code(int lang)
{
    static const char *const codes[SL_LANG_LAST + 1] = { "en", "es", "fr", "de", "he", "ar", "zh", "it", "nl", "pt", "pl", "cs", "ru", "ja" };
    return codes[lang >= 0 && lang <= SL_LANG_LAST ? lang : 0];
}
static int sl_phrase_one_cmp(const void *a, const void *b)
{
    return strcmp(((const struct sl_phrase_one *)a)->en, ((const struct sl_phrase_one *)b)->en);
}

static int sl_phrase_cmp(const void *a, const void *b)
{
    return strcmp(((const struct sl_phrase *)a)->en, ((const struct sl_phrase *)b)->en);
}

static const char *sl_tr_find(const char *s, int lang)
{
    static bool sorted;
    if (lang >= SL_LANG_ZH && lang <= SL_LANG_LAST)
    {
        struct sl_lang_one *t = &sl_one[lang - SL_LANG_ZH];
        if (!t->sorted)
        {
            qsort(t->p, t->n, sizeof t->p[0], sl_phrase_one_cmp);
            t->sorted = true;
        }
        struct sl_phrase_one key = { s, NULL };
        const struct sl_phrase_one *p = bsearch(&key, t->p, t->n, sizeof t->p[0], sl_phrase_one_cmp);
        return p ? p->tr : NULL;
    }
    if (!sorted)
    {
        qsort(sl_phrases, ARRAYLEN(sl_phrases), sizeof sl_phrases[0], sl_phrase_cmp);
        sorted = true;
    }
    struct sl_phrase key = { s, { NULL } };
    const struct sl_phrase *p = bsearch(&key, sl_phrases, ARRAYLEN(sl_phrases), sizeof sl_phrases[0], sl_phrase_cmp);
    return p ? p->t[lang - 1] : NULL;
}
/* an LVGL symbol: U+F000 to U+F8FF, three bytes from EF 80 */
static bool sl_tr_symbol(const unsigned char *u) { return u[0] == 0xef && u[1] >= 0x80 && u[1] <= 0xa3 && u[2]; }
static const char *sl_tr(const char *s)
{
    int lang = global_settings.sloppy_language;
    if (lang <= 0 || lang > SL_LANG_LAST || !s || !s[0]) return s;
    const char *t = sl_tr_find(s, lang);
    if (t) return t;
    /* a symbol and its word, "<play>  Play" or "More  <arrow>": the word
     * translated, the symbol kept; in Hebrew and Arabic an arrow points
     * the way the page reads */
    size_t n = strlen(s);
    const unsigned char *u = (const unsigned char *)s;
    bool lead = n > 3 && sl_tr_symbol(u), trail = n > 3 && sl_tr_symbol(u + n - 3);
    if (!(lead || trail) || n >= 120) return s;
    size_t a = lead ? 3 : 0, b = trail ? n - 3 : n;
    while (a < b && s[a] == ' ') a++;
    while (b > a && s[b - 1] == ' ') b--;
    char word[120];
    if (b <= a || b - a >= sizeof word) return s;
    memcpy(word, s + a, b - a);
    word[b - a] = '\0';
    const char *w = sl_tr_find(word, lang);
    if (!w) return s;
    static char buf[8][200];
    static int k;
    k = (k + 1) & 7;
    snprintf(buf[k], sizeof buf[k], "%.*s%s%s", (int)a, s, w, s + b);
    if (lang == 4 || lang == 5)
        for (char *q = buf[k]; (q = strstr(q, "\xef\x81")) != NULL; q += 3)
            if ((unsigned char)q[2] == 0x93 || (unsigned char)q[2] == 0x94) q[2] = (char)(0x93 + 0x94 - (unsigned char)q[2]);   /* LV_SYMBOL_LEFT and RIGHT */
    return buf[k];
}
/* A count's word ("songs" after 1,234) in the form that number takes.
 * Polish, Czech and Russian have one for 2 to 4 (in Polish and Russian 22
 * to 24 too, never 12 to 14), and Russian says 21, 31 ... as it says 1:
 * the table has the singular and the form for 5 and over; the 2 to 4 one
 * is here. Other languages: the singular for 1, the plural for the rest. */
static const char *sl_tr_count(int n, const char *one, const char *many)
{
    int lang = global_settings.sloppy_language;
    unsigned u = n < 0 ? (unsigned)-n : (unsigned)n, m10 = u % 10, m100 = u % 100;
    if (lang == SL_LANG_RU && m10 == 1 && m100 != 11) return sl_tr(one);
    bool few = lang == SL_LANG_CS ? u >= 2 && u <= 4
             : (lang == SL_LANG_PL || lang == SL_LANG_RU) && m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14);
    if (few)
    {
        static const char *const f[][4] = {   /* English, Polish, Czech, Russian */
            { "songs", "utwory", "skladby", "песни" },
            { "albums", "albumy", "alba", "альбома" },
            { "artists", "wykonawcy", "interpreti", "артиста" },
        };
        for (size_t i = 0; i < ARRAYLEN(f); i++)
            if (!strcmp(many, f[i][0])) return f[i][lang == SL_LANG_PL ? 1 : lang == SL_LANG_CS ? 2 : 3];
    }
    return sl_tr(u == 1 ? one : many);
}
