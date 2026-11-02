/* GabagoolOS search tests: cc -fsanitize=address,undefined search_test.c -lm */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../sloppy_search_state.h"

static int score(const char *query, const char *title, const char *artist, const char *album)
{
    char q[SL_Q_MAX], t[256], a[256], b[256], *w[6];
    sl_q_fold(query, q, sizeof q);
    int n = sl_q_words(q, w);
    sl_q_fold(title, t, sizeof t); sl_q_fold(artist, a, sizeof a); sl_q_fold(album, b, sizeof b);
    return n ? sl_q_score(w, n, t, a, b) : 0;
}

static struct sl_p_template tpl[SL_P_MAX_TEMPLATES];
static int ntpl;

/* Draw a template shape again with jitter, scale, slant and a random
 * stroke order and direction, as a finger would. */
static char redraw(char want, unsigned seed, char best[3])
{
    srand(seed);
    struct sl_pt in[SL_P_MAX_IN];
    int n = 0, stroke = 0;
    const signed char *s = sl_p_shapes;
    int variants = 0;
    for (const signed char *q = sl_p_shapes; *q; )
    {
        variants += *q++ == want;
        while (*q != -2) q += *q == -1 ? 1 : 2;
        q++;
    }
    int variant = 0, pick = (int)(seed % (unsigned)variants);
    while (*s)
    {
        char ch = *s++;
        const signed char *start = s;
        while (*s != -2) s += *s == -1 ? 1 : 2;
        s++;
        if (ch != want || variant++ != pick) continue;
        float scale = 0.6f + (rand() % 100) / 100.0f, slant = ((rand() % 40) - 20) / 100.0f;
        /* collect strokes, then emit them in a random order and direction */
        struct sl_pt strokes[6][32];
        int len[6] = { 0 }, ns = 0;
        for (const signed char *p = start; *p != -2; )
        {
            if (*p == -1) { ns++; p++; continue; }
            if (len[ns] < 32) { strokes[ns][len[ns]].x = p[0]; strokes[ns][len[ns]].y = p[1]; len[ns]++; }
            p += 2;
        }
        ns++;
        int order[6] = { 0, 1, 2, 3, 4, 5 };
        for (int i = ns - 1; i > 0; i--) { int j = rand() % (i + 1); int x = order[i]; order[i] = order[j]; order[j] = x; }
        for (int o = 0; o < ns; o++)
        {
            int k = order[o];
            bool rev = rand() % 2;
            for (int i = 0; i + 1 < len[k]; i++)
                for (int t = 0; t < 10 && n < SL_P_MAX_IN; t++)
                {
                    int a = rev ? len[k] - 1 - i : i, b = rev ? a - 1 : a + 1;
                    float x = strokes[k][a].x + (strokes[k][b].x - strokes[k][a].x) * t / 10.0f;
                    float y = strokes[k][a].y + (strokes[k][b].y - strokes[k][a].y) * t / 10.0f;
                    in[n].x = (x + slant * (100 - y)) * scale + (rand() % 7) - 3;
                    in[n].y = y * scale + (rand() % 7) - 3;
                    in[n].stroke = stroke;
                    n++;
                }
            stroke++;
        }
        break;
    }
    return sl_p_recognize(in, n, tpl, ntpl, best) ? best[0] : 0;
}

int main(void)
{
    /* matching: word starts, case, accents, Hebrew finals, all words needed */
    assert(score("blu hou", "Blue Hours", "Mira Sol", "Blue Hours") > 0);
    assert(score("BLUE", "blue hours", "", "") > score("blue", "", "Blue", ""));
    assert(score("hours", "Blue Hours", "", "") == 12);
    assert(score("ours", "Blue Hours", "", "") == 0);          /* not a word start */
    assert(score("mira night", "Night Tide", "Mira Sol", "") > 0);
    assert(score("mira xyz", "Night Tide", "Mira Sol", "") == 0);
    assert(score("beyonce", "Halo", "Beyonc\xc3\xa9", "") > 0);
    assert(score("cafe", "Caf\xc3\xa9 del Mar", "", "") > 0);
    /* "שלומ" typed with the usual mem finds "שלום" with a final mem */
    assert(score("\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9e", "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d", "", "") > 0);
    assert(score("\xd7\x9c\xd7\x95", "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d", "", "") == 0);   /* mid-word */
    assert(score("\xd7\xa2\xd7\x95\xd7\x9c", "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d \xd7\xa2\xd7\x95\xd7\x9c\xd7\x9d", "", "") > 0);
    assert(score("", "x", "", "") == 0 && score("  ", "x", "", "") == 0);
    char small[5];
    sl_q_fold("\xd7\xa9\xd7\xa9\xd7\xa9", small, sizeof small);
    assert(strlen(small) == 4);                                /* whole characters only */
    sl_q_fold("\xe2\x80\x94x", small, sizeof small);
    assert(!strcmp(small, "\xe2\x80\x94x"));

    /* handwriting: every template recognizes itself, and redrawn letters
     * (jittered, scaled, slanted, any stroke order) mostly come back right */
    ntpl = sl_p_templates(tpl, SL_P_MAX_TEMPLATES);
    assert(ntpl >= 26);
    for (int i = 0; i < ntpl; i++)
    {
        char best[3];
        struct sl_pt pts[SL_P_N];
        for (int k = 0; k < SL_P_N; k++) pts[k] = tpl[i].cloud.p[k];
        assert(sl_p_recognize(pts, SL_P_N, tpl, ntpl, best) >= 1);
        if (best[0] != tpl[i].ch) printf("template %d (%c) reads as %c\n", i, tpl[i].ch, best[0]);
        assert(best[0] == tpl[i].ch);
    }
    int right = 0, in_top3 = 0, total = 0;
    for (char c = 'A'; c <= 'Z'; c++)
        for (unsigned seed = 1; seed <= 20; seed++)
        {
            char best[3] = { 0 };
            char got = redraw(c, seed * 7919u + (unsigned)c, best);
            total++;
            right += got == c;
            in_top3 += best[0] == c || best[1] == c || best[2] == c;
        }
    printf("handwriting: %d/%d first choice, %d/%d in the top three\n", right, total, in_top3, total);
    assert(right * 100 >= total * 85 && in_top3 * 100 >= total * 97);
    char best[3];
    struct sl_pt dot[1] = { { 5, 5, 0 } };
    assert(sl_p_recognize(dot, 1, tpl, ntpl, best) == 0);
    struct sl_pt same[4] = { { 5, 5, 0 }, { 5, 5, 0 }, { 5, 5, 1 }, { 5, 5, 1 } };
    sl_p_recognize(same, 4, tpl, ntpl, best);                  /* a tap: no crash */
    printf("PASS: search matching (case, accents, Hebrew finals, word starts), handwriting\n");
    return 0;
}
