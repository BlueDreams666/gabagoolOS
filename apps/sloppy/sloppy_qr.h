/* GabagoolOS QR codes: text into a QR code the phone's camera opens.
 * Written for GabagoolOS from ISO/IEC 18004 (GPLv2+, as the rest of
 * apps/sloppy): byte mode, error correction level M, versions 1 to 10
 * (up to 213 bytes, a web address with room to spare), the mask with the
 * lowest penalty. No LVGL, no Rockbox, no I/O, no allocation.
 *
 *   struct sl_qr q;
 *   if (sl_qr_make(&q, "http://10.0.0.4:4399")) ... sl_qr_dark(&q, x, y) ...
 */
#ifndef SLOPPY_QR_H
#define SLOPPY_QR_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define SL_QR_MAX_VER 10
#define SL_QR_MAX_SIZE (17 + 4 * SL_QR_MAX_VER)

struct sl_qr {
    int size;                                   /* modules a side: 21 to 57 */
    int version;
    uint8_t m[SL_QR_MAX_SIZE][SL_QR_MAX_SIZE];  /* bit 0 dark, bit 1 a function module (not data) */
};

static inline bool sl_qr_dark(const struct sl_qr *q, int x, int y)
{
    return x >= 0 && y >= 0 && x < q->size && y < q->size && (q->m[y][x] & 1);
}

/* level M: error correction bytes per block, and blocks, by version */
static const uint8_t sl_qr_ecc_m[SL_QR_MAX_VER + 1] = { 0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26 };
static const uint8_t sl_qr_blocks_m[SL_QR_MAX_VER + 1] = { 0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5 };

/* all modules of a version that can hold data (the codewords times 8) */
static inline int sl_qr_raw_modules(int ver)
{
    int r = (16 * ver + 128) * ver + 64;
    if (ver >= 2)
    {
        int align = ver / 7 + 2;
        r -= (25 * align - 10) * align - 55;
        if (ver >= 7) r -= 36;
    }
    return r;
}
static inline int sl_qr_data_bytes(int ver)
{
    return sl_qr_raw_modules(ver) / 8 - sl_qr_ecc_m[ver] * sl_qr_blocks_m[ver];
}

/* GF(256), x^8 + x^4 + x^3 + x^2 + 1 */
static inline uint8_t sl_qr_mul(uint8_t x, uint8_t y)
{
    unsigned z = 0;
    for (int i = 7; i >= 0; i--)
    {
        z = (z << 1) ^ ((z >> 7) * 0x11d);
        z ^= ((y >> i) & 1u) * x;
    }
    return (uint8_t)z;
}
/* the generator of degree n, highest power first (its leading 1 left out) */
static inline void sl_qr_rs_gen(uint8_t *g, int n)
{
    memset(g, 0, (size_t)n);
    g[n - 1] = 1;
    uint8_t root = 1;
    for (int i = 0; i < n; i++)
    {
        for (int j = 0; j < n; j++)
        {
            g[j] = sl_qr_mul(g[j], root);
            if (j + 1 < n) g[j] ^= g[j + 1];
        }
        root = sl_qr_mul(root, 0x02);
    }
}
static inline void sl_qr_rs_rem(const uint8_t *data, int len, const uint8_t *g, int n, uint8_t *out)
{
    memset(out, 0, (size_t)n);
    for (int i = 0; i < len; i++)
    {
        uint8_t f = data[i] ^ out[0];
        memmove(out, out + 1, (size_t)(n - 1));
        out[n - 1] = 0;
        for (int j = 0; j < n; j++) out[j] ^= sl_qr_mul(g[j], f);
    }
}

static inline void sl_qr_set(struct sl_qr *q, int x, int y, bool dark)
{
    q->m[y][x] = (uint8_t)(2 | (dark ? 1 : 0));
}
static inline void sl_qr_finder(struct sl_qr *q, int cx, int cy)
{
    for (int dy = -4; dy <= 4; dy++)
        for (int dx = -4; dx <= 4; dx++)
        {
            int x = cx + dx, y = cy + dy, d = dx < 0 ? -dx : dx, e = dy < 0 ? -dy : dy;
            if (e > d) d = e;
            if (x >= 0 && y >= 0 && x < q->size && y < q->size) sl_qr_set(q, x, y, d != 2 && d != 4);
        }
}
/* the 15 format bits (level M, the mask), around the finders */
static inline void sl_qr_format(struct sl_qr *q, int mask)
{
    int data = (0 << 3) | mask, rem = data;        /* M is 00 */
    for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    int bits = ((data << 10) | rem) ^ 0x5412;
    for (int i = 0; i <= 5; i++) sl_qr_set(q, 8, i, (bits >> i) & 1);
    sl_qr_set(q, 8, 7, (bits >> 6) & 1);
    sl_qr_set(q, 8, 8, (bits >> 7) & 1);
    sl_qr_set(q, 7, 8, (bits >> 8) & 1);
    for (int i = 9; i < 15; i++) sl_qr_set(q, 14 - i, 8, (bits >> i) & 1);
    for (int i = 0; i < 8; i++) sl_qr_set(q, q->size - 1 - i, 8, (bits >> i) & 1);
    for (int i = 8; i < 15; i++) sl_qr_set(q, 8, q->size - 15 + i, (bits >> i) & 1);
    sl_qr_set(q, 8, q->size - 8, true);           /* the dark module */
}
static inline void sl_qr_functions(struct sl_qr *q)
{
    int n = q->size, v = q->version;
    for (int i = 0; i < n; i++) { sl_qr_set(q, 6, i, i % 2 == 0); sl_qr_set(q, i, 6, i % 2 == 0); }
    sl_qr_finder(q, 3, 3);
    sl_qr_finder(q, n - 4, 3);
    sl_qr_finder(q, 3, n - 4);
    if (v >= 2)
    {
        int count = v / 7 + 2, step = (v * 4 + count * 2 + 1) / (count * 2 - 2) * 2, pos[7];
        pos[0] = 6;
        for (int i = count - 1, p = n - 7; i >= 1; i--, p -= step) pos[i] = p;
        for (int i = 0; i < count; i++)
            for (int j = 0; j < count; j++)
            {
                if ((i == 0 && j == 0) || (i == 0 && j == count - 1) || (i == count - 1 && j == 0)) continue;
                for (int dy = -2; dy <= 2; dy++)
                    for (int dx = -2; dx <= 2; dx++)
                    {
                        int d = dx < 0 ? -dx : dx, e = dy < 0 ? -dy : dy;
                        sl_qr_set(q, pos[i] + dx, pos[j] + dy, (d > e ? d : e) != 1);
                    }
            }
    }
    sl_qr_format(q, 0);                           /* reserved now, written with the mask */
    if (v >= 7)
    {
        int rem = v;
        for (int i = 0; i < 12; i++) rem = (rem << 1) ^ ((rem >> 11) * 0x1f25);
        long bits = (long)v << 12 | rem;
        for (int i = 0; i < 18; i++)
        {
            bool b = (bits >> i) & 1;
            int a = n - 11 + i % 3, c = i / 3;
            sl_qr_set(q, a, c, b);
            sl_qr_set(q, c, a, b);
        }
    }
}
static inline bool sl_qr_mask_bit(int mask, int x, int y)
{
    switch (mask)
    {
        case 0: return (x + y) % 2 == 0;
        case 1: return y % 2 == 0;
        case 2: return x % 3 == 0;
        case 3: return (x + y) % 3 == 0;
        case 4: return (x / 3 + y / 2) % 2 == 0;
        case 5: return x * y % 2 + x * y % 3 == 0;
        case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
        default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}
static inline void sl_qr_apply_mask(struct sl_qr *q, int mask)
{
    for (int y = 0; y < q->size; y++)
        for (int x = 0; x < q->size; x++)
            if (!(q->m[y][x] & 2) && sl_qr_mask_bit(mask, x, y)) q->m[y][x] ^= 1;
}
/* the standard's penalty: runs, 2x2 blocks, finder-like patterns, balance */
static inline long sl_qr_penalty(const struct sl_qr *q)
{
    long p = 0;
    int n = q->size, dark = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int a = 0; a < n; a++)
        {
            int run = 0, last = -1;
            unsigned hist = 0;                    /* the last 11 modules, newest lowest */
            for (int b = 0; b < n; b++)
            {
                int c = pass ? (q->m[b][a] & 1) : (q->m[a][b] & 1);
                if (c == last) { if (++run == 5) p += 3; else if (run > 5) p++; }
                else { last = c; run = 1; }
                hist = ((hist << 1) | (unsigned)c) & 0x7ff;
                if (b >= 10 && (hist == 0x5d || hist == 0x5d0)) p += 40;   /* 1011101 with four light on a side */
            }
        }
    for (int y = 0; y + 1 < n; y++)
        for (int x = 0; x + 1 < n; x++)
        {
            int c = q->m[y][x] & 1;
            if (c == (q->m[y][x + 1] & 1) && c == (q->m[y + 1][x] & 1) && c == (q->m[y + 1][x + 1] & 1)) p += 3;
        }
    for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) dark += q->m[y][x] & 1;
    long total = (long)n * n, k = ((long)dark * 20 - total * 10);
    if (k < 0) k = -k;
    p += (k + total - 1) / total * 10 - 10 > 0 ? ((k + total - 1) / total - 1) * 10 : 0;
    return p;
}

/* text into q; false when it is longer than version 10 holds */
static inline bool sl_qr_make(struct sl_qr *q, const char *text)
{
    int len = (int)strlen(text), ver = 1;
    while (ver <= SL_QR_MAX_VER && 4 + (ver < 10 ? 8 : 16) + len * 8 > sl_qr_data_bytes(ver) * 8) ver++;
    if (ver > SL_QR_MAX_VER) return false;
    uint8_t data[400], all[400];
    int cap = sl_qr_data_bytes(ver), bits = 0;
    memset(data, 0, sizeof data);
#define SL_QR_PUT(v, nb) do { for (int i_ = (nb) - 1; i_ >= 0; i_--, bits++) \
        if (((v) >> i_) & 1) data[bits >> 3] |= (uint8_t)(0x80 >> (bits & 7)); } while (0)
    SL_QR_PUT(4, 4);                               /* byte mode */
    SL_QR_PUT(len, ver < 10 ? 8 : 16);
    for (int i = 0; i < len; i++) SL_QR_PUT((uint8_t)text[i], 8);
    int term = cap * 8 - bits < 4 ? cap * 8 - bits : 4;
    bits += term;
    bits = (bits + 7) & ~7;
#undef SL_QR_PUT
    for (int i = bits / 8, pad = 0xec; i < cap; i++, pad ^= 0xec ^ 0x11) data[i] = (uint8_t)pad;
    /* split into blocks, each with its error correction, then interleaved */
    int nb = sl_qr_blocks_m[ver], ecc = sl_qr_ecc_m[ver], raw = sl_qr_raw_modules(ver) / 8;
    int short_n = nb - raw % nb, short_len = raw / nb;      /* short blocks hold a byte less */
    uint8_t gen[32], eccs[5][32];
    sl_qr_rs_gen(gen, ecc);
    int off = 0, k = 0;
    int dlen[5];
    for (int b = 0; b < nb; b++)
    {
        dlen[b] = short_len - ecc + (b < short_n ? 0 : 1);
        sl_qr_rs_rem(data + off, dlen[b], gen, ecc, eccs[b]);
        off += dlen[b];
    }
    for (int i = 0; i <= short_len - ecc; i++)
        for (int b = 0, o = 0; b < nb; o += dlen[b], b++)
            if (i < dlen[b]) all[k++] = data[o + i];
    for (int i = 0; i < ecc; i++)
        for (int b = 0; b < nb; b++) all[k++] = eccs[b][i];
    /* the matrix: the fixed parts, then the codewords zigzagging up and down */
    memset(q, 0, sizeof *q);
    q->version = ver;
    q->size = 17 + 4 * ver;
    sl_qr_functions(q);
    int i = 0, n = q->size;
    for (int right = n - 1; right >= 1; right -= 2)
    {
        if (right == 6) right = 5;
        for (int vert = 0; vert < n; vert++)
            for (int j = 0; j < 2; j++)
            {
                int x = right - j, up = ((right + 1) & 2) == 0, y = up ? n - 1 - vert : vert;
                if (!(q->m[y][x] & 2) && i < k * 8)
                {
                    if ((all[i >> 3] >> (7 - (i & 7))) & 1) q->m[y][x] |= 1;
                    i++;
                }
            }
    }
    /* the mask with the lowest penalty */
    int best = 0;
    long best_p = -1;
    for (int mask = 0; mask < 8; mask++)
    {
        sl_qr_apply_mask(q, mask);
        sl_qr_format(q, mask);
        long p = sl_qr_penalty(q);
        if (best_p < 0 || p < best_p) { best_p = p; best = mask; }
        sl_qr_apply_mask(q, mask);                /* undone: XOR twice */
    }
    sl_qr_apply_mask(q, best);
    sl_qr_format(q, best);
    return true;
}
#endif
