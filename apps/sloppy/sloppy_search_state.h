/* GabagoolOS search: text matching and a handwriting recognizer for single
 * letters. No LVGL or Rockbox. GPLv2+.
 *
 * Matching folds case, common Latin accents and Hebrew final letters, and
 * wants every word of the query to start a word in the song's title, artist
 * or album ("blu hou" finds "Blue Hours").
 *
 * Handwriting uses the $P point-cloud recognizer (Vatavu, Anthony and
 * Wobbrock, 2012): the strokes become one cloud of points, and the closest
 * letter template wins. Stroke order and direction don't matter, so a
 * letter can be drawn the way a person learned it. */
#ifndef SLOPPY_SEARCH_STATE_H
#define SLOPPY_SEARCH_STATE_H
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------ matching */
#define SL_Q_MAX 64

#include "sloppy_fold.h"

/* Fold a whole string into out (at most size-1 bytes, whole characters). */
static inline int sl_q_fold(const char *s, char *out, int size)
{
    int at = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p)
    {
        unsigned char f[4];
        int fl;
        int used = sl_q_fold_char(p, f, &fl);
        if (at + fl > size - 1) break;
        memcpy(out + at, f, (size_t)fl);
        at += fl;
        p += used;
    }
    out[at] = 0;
    return at;
}

static inline bool sl_q_word_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80;
}

/* Split a folded query into words. Returns the count (at most 6). */
static inline int sl_q_words(char *q, char *words[6])
{
    int n = 0;
    char *p = q;
    while (*p && n < 6)
    {
        while (*p && !sl_q_word_char((unsigned char)*p)) p++;
        if (!*p) break;
        words[n++] = p;
        while (*p && sl_q_word_char((unsigned char)*p)) p++;
        if (*p) *p++ = 0;
    }
    return n;
}

/* Score of word w in folded text: 3 at the very start, 2 at a word start,
 * 0 when it starts no word. Bytes of 0x80 and up count as letters, so a
 * match inside a Hebrew or accented word is not a word start. */
static inline int sl_q_word_score(const char *text, const char *w)
{
    for (const char *p = text; (p = strstr(p, w)) != NULL; p++)
    {
        if (p == text) return 3;
        if (!sl_q_word_char((unsigned char)p[-1])) return 2;
    }
    return 0;
}

/* How well the folded fields match every query word; 0 = no match.
 * The title counts most, then the artist, then the album. */
static inline int sl_q_score(char *const words[], int n_words,
                             const char *title, const char *artist, const char *album)
{
    int total = 0;
    for (int i = 0; i < n_words; i++)
    {
        int t = sl_q_word_score(title, words[i]);
        int a = sl_q_word_score(artist, words[i]);
        int b = sl_q_word_score(album, words[i]);
        int best = t ? 10 + t : a ? 5 + a : b ? 2 + b : 0;
        if (!best) return 0;
        total += best;
    }
    return total;
}

/* ------------------------------------------------------- handwriting */
#define SL_P_N 32            /* points per cloud */
#define SL_P_MAX_IN 600      /* raw touch points kept */

struct sl_pt { float x, y; int stroke; };
struct sl_p_cloud { struct sl_pt p[SL_P_N]; };

static inline float sl_p_dist(struct sl_pt a, struct sl_pt b)
{
    float dx = a.x - b.x, dy = a.y - b.y;
    return sqrtf(dx * dx + dy * dy);
}

/* Resample the strokes to SL_P_N points, scale to a unit box keeping the
 * shape, and centre on the centroid. */
static inline bool sl_p_normalize(const struct sl_pt *in, int n, struct sl_p_cloud *out)
{
    if (n < 2) return false;
    float len = 0;
    for (int i = 1; i < n; i++)
        if (in[i].stroke == in[i - 1].stroke) len += sl_p_dist(in[i - 1], in[i]);
    float step = len / (SL_P_N - 1), acc = 0;
    int k = 0;
    struct sl_pt prev = in[0];
    out->p[k++] = in[0];
    for (int i = 1; i < n && k < SL_P_N; i++)
    {
        if (in[i].stroke != in[i - 1].stroke) { prev = in[i]; continue; }
        float d = sl_p_dist(prev, in[i]);
        while (step > 0 && acc + d >= step && k < SL_P_N)
        {
            float t = (step - acc) / d;
            struct sl_pt q = { prev.x + t * (in[i].x - prev.x), prev.y + t * (in[i].y - prev.y), in[i].stroke };
            out->p[k++] = q;
            prev = q;
            d = sl_p_dist(prev, in[i]);
            acc = 0;
        }
        acc += d;
        prev = in[i];
    }
    while (k < SL_P_N) out->p[k++] = in[n - 1];
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    for (int i = 0; i < SL_P_N; i++)
    {
        if (out->p[i].x < minx) minx = out->p[i].x;
        if (out->p[i].y < miny) miny = out->p[i].y;
        if (out->p[i].x > maxx) maxx = out->p[i].x;
        if (out->p[i].y > maxy) maxy = out->p[i].y;
    }
    float size = fmaxf(maxx - minx, maxy - miny);
    if (size < 1e-3f) size = 1e-3f;
    float cx = 0, cy = 0;
    for (int i = 0; i < SL_P_N; i++)
    {
        out->p[i].x = (out->p[i].x - minx) / size;
        out->p[i].y = (out->p[i].y - miny) / size;
        cx += out->p[i].x; cy += out->p[i].y;
    }
    cx /= SL_P_N; cy /= SL_P_N;
    for (int i = 0; i < SL_P_N; i++) { out->p[i].x -= cx; out->p[i].y -= cy; }
    return true;
}

static inline float sl_p_cloud_distance(const struct sl_p_cloud *a, const struct sl_p_cloud *b, int start)
{
    bool matched[SL_P_N] = { false };
    float sum = 0;
    int i = start;
    do
    {
        int index = -1;
        float min = 1e9f;
        for (int j = 0; j < SL_P_N; j++)
            if (!matched[j])
            {
                float d = sl_p_dist(a->p[i], b->p[j]);
                if (d < min) { min = d; index = j; }
            }
        matched[index] = true;
        float weight = 1 - (float)((i - start + SL_P_N) % SL_P_N) / SL_P_N;
        sum += weight * min;
        i = (i + 1) % SL_P_N;
    } while (i != start);
    return sum;
}

static inline float sl_p_match(const struct sl_p_cloud *a, const struct sl_p_cloud *b)
{
    int step = (int)floorf(sqrt((double)SL_P_N));
    float min = 1e9f;
    for (int i = 0; i < SL_P_N; i += step)
    {
        float d1 = sl_p_cloud_distance(a, b, i), d2 = sl_p_cloud_distance(b, a, i);
        if (d1 < min) min = d1;
        if (d2 < min) min = d2;
    }
    return min;
}

/* Letter templates as strokes on a 100 x 100 grid, y down. Each stroke is
 * a list of points ending with -1; a template ends with -2. Variants cover
 * common ways to write a letter. */
static const signed char sl_p_shapes[] = {
    'A', 10,100, 50,0, 90,100, -1, 28,60, 72,60, -2,
    'A', 10,100, 50,0, 90,100, 70,60, 30,60, -2,
    'B', 20,0, 20,100, -1, 20,0, 62,3, 78,20, 64,44, 20,50, -1, 20,50, 68,54, 84,74, 68,96, 20,100, -2,
    'C', 85,18, 65,3, 40,3, 18,20, 10,50, 18,80, 40,97, 65,97, 85,82, -2,
    'D', 20,0, 20,100, -1, 20,0, 58,4, 82,25, 88,50, 82,75, 58,96, 20,100, -2,
    'E', 80,0, 20,0, 20,100, 80,100, -1, 20,50, 70,50, -2,
    'E', 80,5, 55,0, 25,8, 25,40, 50,50, 20,60, 20,92, 50,100, 82,95, -2,
    'F', 80,0, 20,0, 20,100, -1, 20,50, 65,50, -2,
    'G', 85,18, 65,3, 40,3, 18,20, 10,50, 18,80, 40,97, 65,97, 85,82, 85,58, 58,58, -2,
    'H', 20,0, 20,100, -1, 80,0, 80,100, -1, 20,50, 80,50, -2,
    'I', 50,0, 50,100, -2,
    'I', 25,0, 75,0, -1, 50,0, 50,100, -1, 25,100, 75,100, -2,
    'J', 72,0, 72,72, 64,92, 46,100, 28,94, 20,78, -2,
    'K', 20,0, 20,100, -1, 80,0, 20,58, -1, 42,40, 82,100, -2,
    'K', 20,0, 20,100, -1, 80,0, 22,55, 80,100, -2,
    'L', 20,0, 20,100, 80,100, -2,
    'M', 12,100, 12,0, 50,62, 88,0, 88,100, -2,
    'N', 20,100, 20,0, 80,100, 80,0, -2,
    'O', 50,0, 22,10, 8,35, 8,65, 22,90, 50,100, 78,90, 92,65, 92,35, 78,10, 50,0, -2,
    'P', 20,0, 20,100, -1, 20,0, 64,4, 80,24, 64,46, 20,50, -2,
    'P', 20,100, 20,0, 64,4, 80,24, 64,46, 20,50, -2,
    'Q', 50,0, 22,10, 8,35, 8,65, 22,90, 50,100, 78,90, 92,65, 92,35, 78,10, 50,0, -1, 60,70, 95,105, -2,
    'R', 20,0, 20,100, -1, 20,0, 64,4, 80,24, 64,46, 20,50, 80,100, -2,
    'R', 20,100, 20,0, 64,4, 80,24, 64,46, 20,50, -1, 45,50, 82,100, -2,
    'S', 80,14, 62,2, 36,2, 18,18, 26,40, 50,50, 74,60, 84,80, 66,98, 36,98, 16,84, -2,
    'T', 10,0, 90,0, -1, 50,0, 50,100, -2,
    'U', 18,0, 18,68, 30,92, 50,100, 70,92, 82,68, 82,0, -2,
    'V', 14,0, 50,100, 86,0, -2,
    'W', 4,0, 27,100, 50,28, 73,100, 96,0, -2,
    'X', 15,0, 85,100, -1, 85,0, 15,100, -2,
    'Y', 15,0, 50,48, -1, 85,0, 50,48, 50,100, -2,
    'Y', 15,0, 50,48, 85,0, -1, 50,48, 50,100, -2,
    'Z', 15,0, 85,0, 15,100, 85,100, -2,
    0
};

#define SL_P_MAX_TEMPLATES 40
struct sl_p_template { char ch; struct sl_p_cloud cloud; };

/* Build the template clouds once. Returns how many there are. */
static inline int sl_p_templates(struct sl_p_template *out, int max)
{
    int n = 0;
    const signed char *s = sl_p_shapes;
    while (*s && n < max)
    {
        char ch = *s++;
        struct sl_pt pts[64];
        int k = 0, stroke = 0;
        while (*s != -2)
        {
            if (*s == -1) { stroke++; s++; continue; }
            if (k < 64) { pts[k].x = s[0]; pts[k].y = s[1]; pts[k].stroke = stroke; k++; }
            s += 2;
        }
        s++;
        /* straight segments: add in-between points so resampling sees the lines */
        struct sl_pt dense[256];
        int m = 0;
        for (int i = 0; i < k && m < 256; i++)
        {
            if (i && pts[i].stroke == pts[i - 1].stroke)
                for (int t = 1; t < 8 && m < 256; t++)
                {
                    struct sl_pt q = { pts[i - 1].x + (pts[i].x - pts[i - 1].x) * t / 8,
                                       pts[i - 1].y + (pts[i].y - pts[i - 1].y) * t / 8, pts[i].stroke };
                    dense[m++] = q;
                }
            if (m < 256) dense[m++] = pts[i];
        }
        out[n].ch = ch;
        if (sl_p_normalize(dense, m, &out[n].cloud)) n++;
    }
    return n;
}

/* The best letters for the strokes, best first; returns how many (<= 3). */
static inline int sl_p_recognize(const struct sl_pt *in, int n, const struct sl_p_template *t,
                                 int nt, char best[3])
{
    struct sl_p_cloud c;
    if (!sl_p_normalize(in, n, &c)) return 0;
    float score[3] = { 1e9f, 1e9f, 1e9f };
    int found = 0;
    for (int i = 0; i < nt; i++)
    {
        float d = sl_p_match(&c, &t[i].cloud);
        /* a letter appears once, at its best variant */
        int have = -1;
        for (int k = 0; k < found; k++) if (best[k] == t[i].ch) have = k;
        if (have >= 0)
        {
            if (d >= score[have]) continue;
            for (int k = have; k < found - 1; k++) { best[k] = best[k + 1]; score[k] = score[k + 1]; }
            found--;
        }
        int at = found;
        while (at > 0 && d < score[at - 1]) at--;
        if (at >= 3) continue;
        for (int k = (found < 3 ? found : 2); k > at; k--) { best[k] = best[k - 1]; score[k] = score[k - 1]; }
        best[at] = t[i].ch;
        score[at] = d;
        if (found < 3) found++;
    }
    return found;
}
#endif
