/* Swipe typing decoder: cc -fsanitize=address,undefined swipetype_test.c -lm */
#include <assert.h>
#include <stdio.h>
#include "../sloppy_swipe_type.h"

/* the search keyboard's QWERTY: 10 keys of 40 px, 6 px apart, rows centred */
static struct st_layout qwerty(void)
{
    static const char *rows[3] = { "qwertyuiop", "asdfghjkl", "zxcvbnm" };
    struct st_layout l = { .n = 0, .key_w = 46, .key_h = 64 };
    for (int r = 0; r < 3; r++)
    {
        int n = (int)strlen(rows[r]);
        float row_w = n * 40 + (n - 1) * 6, x0 = (456 - row_w) / 2 + 20;
        for (int i = 0; i < n; i++)
            l.k[l.n++] = (struct st_key){ (uint32_t)rows[r][i], x0 + i * 46, 29 + r * 64 };
    }
    return l;
}

static unsigned rng = 12345;
static float jitter(float amp) { rng = rng * 1103515245u + 12345u; return ((rng >> 8) % 1000 / 1000.0f - 0.5f) * 2 * amp; }

/* a finger's path for a word: through each key with wobble, corners cut */
static int finger(const struct st_layout *l, const char *word, float *px, float *py, float wobble)
{
    int n = 0;
    float lx = 0, ly = 0;
    for (const char *s = word; *s; s++)
    {
        int k = st_key_of(l, (uint32_t)*s);
        float x = l->k[k].x + jitter(wobble), y = l->k[k].y + jitter(wobble);
        if (n)
            for (int t = 1; t < 6; t++)
            { px[n] = lx + (x - lx) * t / 6 + jitter(3); py[n] = ly + (y - ly) * t / 6 + jitter(3); n++; }
        px[n] = x; py[n] = y; n++;
        lx = x; ly = y;
    }
    return n;
}

int main(void)
{
    struct st_layout l = qwerty();
    static struct st_word words[512]; static char arena[8192]; static int hash[1024];
    for (int i = 0; i < 1024; i++) hash[i] = -1;
    struct st_vocab v = { words, 0, 512, arena, 0, sizeof arena, hash, 1024 };
    const char *lib[] = { "Night Tide", "Fade Into You", "Come Back to Earth", "The Slow Rush", "Breathe Deeper",
        "Radio", "Fix You", "Pumped Up Kicks", "Silent City", "Blue Hours", "Mazzy Star", "Tame Impala",
        "Mac Miller", "Swimming", "night might right light sight fight tight", "the they then them there their",
        "hello help held hell", "love live life like line", "Don't Stop Me Now", "café (no)", "volume step",
        "bluetooth wifi equalizer crossfade", NULL };
    for (int i = 0; lib[i]; i++) assert(st_vocab_add(&l, &v, lib[i]));
    int n = v.n;
    int night = -1, cafe = -1, dont = -1;
    for (int i = 0; i < n; i++)
    {
        if (!strcmp(arena + words[i].off, "night")) night = i;
        if (!strcmp(arena + words[i].off, "caf\xc3\xa9")) cafe = i;
        if (!strcmp(arena + words[i].off, "don't")) dont = i;
    }
    assert(night >= 0 && v.w[night].count == 2 && cafe >= 0 && dont >= 0);   /* counted; é typed on e; "don't" kept whole */
    int c3[3]; assert(st_complete(&v, "cafe", c3, 3) == 1 && c3[0] == cafe);  /* typed plain, found accented */

    const char *want[] = { "night", "fade", "earth", "breathe", "radio", "silent", "city", "blue", "hours",
                           "mazzy", "star", "impala", "swimming", "light", "they", "there", "hello", "love",
                           "life", "volume", "step", "bluetooth", "equalizer", "crossfade", "pumped", "kicks", NULL };
    float px[400], py[400];
    int ok = 0, total = 0, top3 = 0;
    for (int round = 0; round < 6; round++)
        for (int i = 0; want[i]; i++)
        {
            int m = finger(&l, want[i], px, py, 10 + round * 2);
            struct st_match best[ST_BEST];
            int k = st_decode(&l, &v, px, py, m, best);
            total++;
            if (k && !strcmp(arena + words[best[0].word].off, want[i])) ok++;
            for (int j = 0; j < k && j < 3; j++) if (!strcmp(arena + words[best[j].word].off, want[i])) { top3++; break; }
            if (round == 5 && (!k || strcmp(arena + words[best[0].word].off, want[i])))
                printf("  %s -> %s\n", want[i], k ? arena + words[best[0].word].off : "(none)");
        }
    printf("glides: %d of %d first, %d in the top three\n", ok, total, top3);
    assert(ok * 100 >= total * 85 && top3 * 100 >= total * 97);
    /* a tap is not a glide; a stroke across keys is */
    float tx[3] = { 100, 103, 101 }, ty[3] = { 30, 31, 33 };
    assert(!st_is_swipe(tx, ty, 3, 46));
    int m = finger(&l, "night", px, py, 0);
    assert(st_is_swipe(px, py, m, 46));
    /* Hebrew: the same, by code point */
    struct st_layout h = { .n = 0, .key_w = 46, .key_h = 64 };
    const char *heb = "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d";   /* shalom */
    const char *q = heb; uint32_t cps[8]; int nc = 0; uint32_t c;
    while ((c = st_utf8_next(&q))) cps[nc++] = c;
    for (int i = 0; i < nc; i++) h.k[h.n++] = (struct st_key){ cps[i], 40.0f + i * 92, 30.0f + (i % 2) * 64 };
    h.k[h.n++] = (struct st_key){ 0x5d0, 400, 160 };
    static struct st_word hw[8]; static char ha[64]; int hh[16]; for (int i = 0; i < 16; i++) hh[i] = -1;
    struct st_vocab hv = { hw, 0, 8, ha, 0, sizeof ha, hh, 16 };
    assert(st_vocab_add(&h, &hv, heb) && hv.n == 1 && !strcmp(ha + hw[0].off, heb));
    float hx[8], hy[8];
    for (int i = 0; i < nc; i++) { hx[i] = h.k[i].x + 3; hy[i] = h.k[i].y - 2; }
    struct st_match hb[ST_BEST];
    assert(st_decode(&h, &hv, hx, hy, nc, hb) == 1 && hb[0].word == 0);
    printf("PASS: swipe typing: vocabulary, glides decoded, taps told apart, Hebrew.\n");
    return 0;
}
