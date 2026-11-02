/* GabagoolOS: one character folded for matching and sorting (case, common
 * Latin accents, Hebrew final letters). No LVGL or Rockbox. GPLv2+. */
#ifndef SLOPPY_FOLD_H
#define SLOPPY_FOLD_H
#include <string.h>

/* Fold one character at s (UTF-8) into out (up to 4 bytes). Returns the
 * bytes consumed; *out_len gets the folded length. */
static inline int sl_q_fold_char(const unsigned char *s, unsigned char *out, int *out_len)
{
    unsigned char c = s[0];
    if (c < 0x80)
    {
        out[0] = (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32) : c;
        *out_len = 1;
        return 1;
    }
    if (c == 0xc3 && s[1] >= 0x80 && s[1] <= 0xbf)
    {
        /* Latin-1 letters: accented forms to their plain letter */
        static const char base[] = "aaaaaaaceeeeiiiidnooooo*ouuuuyts"   /* C3 80..9F */
                                   "aaaaaaaceeeeiiiidnooooo/ouuuuyty";  /* C3 A0..BF */
        char b = base[s[1] - 0x80];
        if (b != '*' && b != '/') { out[0] = (unsigned char)b; *out_len = 1; return 2; }
    }
    int n = (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 1;
    for (int i = 1; i < n; i++) if ((s[i] & 0xc0) != 0x80) { n = i; break; }
    memcpy(out, s, (size_t)n);
    /* Hebrew final letters to their usual form */
    if (n == 2 && c == 0xd7 && (s[1] == 0x9a || s[1] == 0x9d || s[1] == 0x9f || s[1] == 0xa3 || s[1] == 0xa5))
        out[1]++;
    *out_len = n;
    return n;
}
#endif
