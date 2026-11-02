/* Chess rules, a small alpha-beta opponent and a saveable game record.
 * Pure C, no UI or filesystem dependencies. GPLv2+. */
#ifndef SLOPPY_CHESS_STATE_H
#define SLOPPY_CHESS_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* 0x88 board: square = rank * 16 + file, rank 0 is White's back rank. */
enum { CH_EMPTY = 0, CH_P = 1, CH_N, CH_B, CH_R, CH_Q, CH_K };
enum { CH_WK = 1, CH_WQ = 2, CH_BK = 4, CH_BQ = 8 };
enum { CHF_CAPTURE = 1, CHF_EP = 2, CHF_CASTLE = 4, CHF_DOUBLE = 8 };
enum sl_chess_status { CH_PLAYING, CH_CHECKMATE, CH_STALEMATE, CH_DRAW_FIFTY,
                       CH_DRAW_MATERIAL, CH_DRAW_REPETITION, CH_DRAW_LENGTH };

#define CH_SQ(file, rank) ((rank) * 16 + (file))
#define CH_FILE(sq) ((sq) & 7)
#define CH_RANK(sq) ((sq) >> 4)
#define CH_ONBOARD(sq) (!((sq) & 0x88))
#define CH_MAX_MOVES 256
#define CH_MAX_PLIES 600
#define CH_MATE 30000

struct sl_chess_pos {
    int8_t board[128];
    int8_t side;          /* 1 white to move, -1 black */
    uint8_t castle;
    int16_t ep;           /* en passant target square or -1 */
    uint16_t halfmove, fullmove;
    uint8_t king[2];      /* [0] white, [1] black */
};

struct sl_chess_move {
    uint8_t from, to;
    int8_t promo;         /* piece type or 0 */
    uint8_t flags;
};

struct sl_chess_undo {
    int8_t captured;
    uint8_t castle;
    int16_t ep;
    uint16_t halfmove;
};

static inline int sl_chess_kidx(int side) { return side > 0 ? 0 : 1; }

static inline void sl_chess_init(struct sl_chess_pos *p)
{
    static const int8_t back[8] = { CH_R, CH_N, CH_B, CH_Q, CH_K, CH_B, CH_N, CH_R };
    memset(p, 0, sizeof *p);
    for (int f = 0; f < 8; f++)
    {
        p->board[CH_SQ(f, 0)] = back[f];
        p->board[CH_SQ(f, 1)] = CH_P;
        p->board[CH_SQ(f, 6)] = -CH_P;
        p->board[CH_SQ(f, 7)] = -back[f];
    }
    p->side = 1; p->castle = CH_WK | CH_WQ | CH_BK | CH_BQ; p->ep = -1;
    p->fullmove = 1; p->king[0] = CH_SQ(4, 0); p->king[1] = CH_SQ(4, 7);
}

/* Minimal FEN reader for tests and puzzles (board, side, castling, ep). */
static inline bool sl_chess_from_fen(struct sl_chess_pos *p, const char *fen)
{
    memset(p, 0, sizeof *p);
    p->ep = -1; p->fullmove = 1;
    int rank = 7, file = 0;
    const char *s = fen;
    for (; *s && *s != ' '; s++)
    {
        if (*s == '/') { rank--; file = 0; continue; }
        if (*s >= '1' && *s <= '8') { file += *s - '0'; continue; }
        const char *names = "pnbrqk";
        char lower = *s >= 'A' && *s <= 'Z' ? *s - 'A' + 'a' : *s;
        const char *at = strchr(names, lower);
        if (!at || file > 7 || rank < 0) return false;
        int type = (int)(at - names) + 1;
        int sq = CH_SQ(file, rank);
        p->board[sq] = (int8_t)(*s == lower ? -type : type);
        if (type == CH_K) p->king[*s == lower ? 1 : 0] = (uint8_t)sq;
        file++;
    }
    if (*s == ' ') s++;
    p->side = *s == 'b' ? -1 : 1;
    while (*s && *s != ' ') s++;
    if (*s == ' ') s++;
    for (; *s && *s != ' '; s++)
        p->castle |= *s == 'K' ? CH_WK : *s == 'Q' ? CH_WQ : *s == 'k' ? CH_BK : *s == 'q' ? CH_BQ : 0;
    if (*s == ' ') s++;
    if (*s >= 'a' && *s <= 'h' && s[1] >= '1' && s[1] <= '8')
        p->ep = CH_SQ(*s - 'a', s[1] - '1');
    return true;
}

static const int8_t ch_knight[8] = { 33, 31, 18, 14, -33, -31, -18, -14 };
static const int8_t ch_king[8] = { 1, -1, 16, -16, 17, 15, -17, -15 };
static const int8_t ch_diag[4] = { 17, 15, -17, -15 };
static const int8_t ch_line[4] = { 1, -1, 16, -16 };

static inline bool sl_chess_attacked(const struct sl_chess_pos *p, int sq, int by)
{
    /* Pawns attack diagonally forward from their own point of view. */
    int pawn_from = by > 0 ? -16 : 16;
    for (int d = -1; d <= 1; d += 2)
    {
        int s = sq + pawn_from + d;
        if (CH_ONBOARD(s) && p->board[s] == by * CH_P) return true;
    }
    for (int i = 0; i < 8; i++)
    {
        int s = sq + ch_knight[i];
        if (CH_ONBOARD(s) && p->board[s] == by * CH_N) return true;
        s = sq + ch_king[i];
        if (CH_ONBOARD(s) && p->board[s] == by * CH_K) return true;
    }
    for (int i = 0; i < 4; i++)
    {
        for (int s = sq + ch_diag[i]; CH_ONBOARD(s); s += ch_diag[i])
        {
            int8_t pc = p->board[s];
            if (!pc) continue;
            if (pc == by * CH_B || pc == by * CH_Q) return true;
            break;
        }
        for (int s = sq + ch_line[i]; CH_ONBOARD(s); s += ch_line[i])
        {
            int8_t pc = p->board[s];
            if (!pc) continue;
            if (pc == by * CH_R || pc == by * CH_Q) return true;
            break;
        }
    }
    return false;
}

static inline bool sl_chess_in_check(const struct sl_chess_pos *p, int side)
{
    return sl_chess_attacked(p, p->king[sl_chess_kidx(side)], -side);
}

static inline void ch_add(struct sl_chess_move *m, int *n, int from, int to, int promo, int flags)
{
    m[*n].from = (uint8_t)from; m[*n].to = (uint8_t)to;
    m[*n].promo = (int8_t)promo; m[*n].flags = (uint8_t)flags; (*n)++;
}

/* Pseudo-legal moves; the king may be left in check. */
static inline int sl_chess_pseudo(const struct sl_chess_pos *p, struct sl_chess_move *m, bool captures_only)
{
    int n = 0, us = p->side;
    for (int from = 0; from < 128; from++)
    {
        if (!CH_ONBOARD(from)) { from += 7; continue; }
        int8_t pc = p->board[from];
        if (!pc || (pc > 0) != (us > 0)) continue;
        int type = pc > 0 ? pc : -pc;
        if (type == CH_P)
        {
            int fwd = us > 0 ? 16 : -16, start = us > 0 ? 1 : 6, last = us > 0 ? 7 : 0;
            int to = from + fwd;
            if (CH_ONBOARD(to) && !p->board[to] && !captures_only)
            {
                if (CH_RANK(to) == last)
                    for (int pr = CH_Q; pr >= CH_N; pr--) ch_add(m, &n, from, to, pr, 0);
                else
                {
                    ch_add(m, &n, from, to, 0, 0);
                    if (CH_RANK(from) == start && !p->board[to + fwd])
                        ch_add(m, &n, from, to + fwd, 0, CHF_DOUBLE);
                }
            }
            for (int d = -1; d <= 1; d += 2)
            {
                to = from + fwd + d;
                if (!CH_ONBOARD(to)) continue;
                if (p->board[to] && (p->board[to] > 0) != (us > 0))
                {
                    if (CH_RANK(to) == last)
                        for (int pr = CH_Q; pr >= CH_N; pr--) ch_add(m, &n, from, to, pr, CHF_CAPTURE);
                    else ch_add(m, &n, from, to, 0, CHF_CAPTURE);
                }
                else if (to == p->ep) ch_add(m, &n, from, to, 0, CHF_CAPTURE | CHF_EP);
            }
            continue;
        }
        if (type == CH_N || type == CH_K)
        {
            const int8_t *dirs = type == CH_N ? ch_knight : ch_king;
            for (int i = 0; i < 8; i++)
            {
                int to = from + dirs[i];
                if (!CH_ONBOARD(to)) continue;
                int8_t t = p->board[to];
                if (!t) { if (!captures_only) ch_add(m, &n, from, to, 0, 0); }
                else if ((t > 0) != (us > 0)) ch_add(m, &n, from, to, 0, CHF_CAPTURE);
            }
            if (type == CH_K && !captures_only)
            {
                int rank = us > 0 ? 0 : 7;
                int ks = us > 0 ? CH_WK : CH_BK, qs = us > 0 ? CH_WQ : CH_BQ;
                if (from == CH_SQ(4, rank) && !sl_chess_attacked(p, from, -us))
                {
                    if ((p->castle & ks) && !p->board[CH_SQ(5, rank)] && !p->board[CH_SQ(6, rank)] &&
                        p->board[CH_SQ(7, rank)] == us * CH_R &&
                        !sl_chess_attacked(p, CH_SQ(5, rank), -us) && !sl_chess_attacked(p, CH_SQ(6, rank), -us))
                        ch_add(m, &n, from, CH_SQ(6, rank), 0, CHF_CASTLE);
                    if ((p->castle & qs) && !p->board[CH_SQ(3, rank)] && !p->board[CH_SQ(2, rank)] &&
                        !p->board[CH_SQ(1, rank)] && p->board[CH_SQ(0, rank)] == us * CH_R &&
                        !sl_chess_attacked(p, CH_SQ(3, rank), -us) && !sl_chess_attacked(p, CH_SQ(2, rank), -us))
                        ch_add(m, &n, from, CH_SQ(2, rank), 0, CHF_CASTLE);
                }
            }
            continue;
        }
        const int8_t *dirs = type == CH_B ? ch_diag : ch_line;
        int ndirs = type == CH_Q ? 8 : 4;
        for (int i = 0; i < ndirs; i++)
        {
            int dir = type == CH_Q ? (i < 4 ? ch_diag[i] : ch_line[i - 4]) : dirs[i];
            for (int to = from + dir; CH_ONBOARD(to); to += dir)
            {
                int8_t t = p->board[to];
                if (!t) { if (!captures_only) ch_add(m, &n, from, to, 0, 0); continue; }
                if ((t > 0) != (us > 0)) ch_add(m, &n, from, to, 0, CHF_CAPTURE);
                break;
            }
        }
    }
    return n;
}

static inline void ch_clear_rook_rights(struct sl_chess_pos *p, int sq)
{
    if (sq == CH_SQ(0, 0)) p->castle &= ~CH_WQ;
    else if (sq == CH_SQ(7, 0)) p->castle &= ~CH_WK;
    else if (sq == CH_SQ(0, 7)) p->castle &= ~CH_BQ;
    else if (sq == CH_SQ(7, 7)) p->castle &= ~CH_BK;
}

static inline void sl_chess_make(struct sl_chess_pos *p, const struct sl_chess_move *mv, struct sl_chess_undo *u)
{
    int us = p->side, from = mv->from, to = mv->to;
    int8_t pc = p->board[from];
    u->castle = p->castle; u->ep = p->ep; u->halfmove = p->halfmove;
    u->captured = p->board[to];
    if (mv->flags & CHF_EP)
    {
        int victim = to + (us > 0 ? -16 : 16);
        u->captured = p->board[victim];
        p->board[victim] = 0;
    }
    p->board[to] = mv->promo ? (int8_t)(us * mv->promo) : pc;
    p->board[from] = 0;
    if (mv->flags & CHF_CASTLE)
    {
        int rank = CH_RANK(from);
        if (CH_FILE(to) == 6) { p->board[CH_SQ(5, rank)] = p->board[CH_SQ(7, rank)]; p->board[CH_SQ(7, rank)] = 0; }
        else { p->board[CH_SQ(3, rank)] = p->board[CH_SQ(0, rank)]; p->board[CH_SQ(0, rank)] = 0; }
    }
    int type = pc > 0 ? pc : -pc;
    if (type == CH_K)
    {
        p->king[sl_chess_kidx(us)] = (uint8_t)to;
        p->castle &= us > 0 ? ~(CH_WK | CH_WQ) : ~(CH_BK | CH_BQ);
    }
    ch_clear_rook_rights(p, from);
    ch_clear_rook_rights(p, to);
    p->ep = (mv->flags & CHF_DOUBLE) ? (int16_t)((from + to) / 2) : -1;
    p->halfmove = (type == CH_P || u->captured) ? 0 : p->halfmove + 1;
    if (us < 0) p->fullmove++;
    p->side = (int8_t)-us;
}

static inline void sl_chess_unmake(struct sl_chess_pos *p, const struct sl_chess_move *mv, const struct sl_chess_undo *u)
{
    int us = -p->side, from = mv->from, to = mv->to;
    p->side = (int8_t)us;
    if (us < 0) p->fullmove--;
    int8_t moved = mv->promo ? (int8_t)(us * CH_P) : p->board[to];
    p->board[from] = moved;
    p->board[to] = (mv->flags & CHF_EP) ? 0 : u->captured;
    if (mv->flags & CHF_EP) p->board[to + (us > 0 ? -16 : 16)] = u->captured;
    if (mv->flags & CHF_CASTLE)
    {
        int rank = CH_RANK(from);
        if (CH_FILE(to) == 6) { p->board[CH_SQ(7, rank)] = p->board[CH_SQ(5, rank)]; p->board[CH_SQ(5, rank)] = 0; }
        else { p->board[CH_SQ(0, rank)] = p->board[CH_SQ(3, rank)]; p->board[CH_SQ(3, rank)] = 0; }
    }
    if ((moved > 0 ? moved : -moved) == CH_K) p->king[sl_chess_kidx(us)] = (uint8_t)from;
    p->castle = u->castle; p->ep = u->ep; p->halfmove = u->halfmove;
}

static inline int sl_chess_legal(struct sl_chess_pos *p, struct sl_chess_move *out, bool captures_only)
{
    struct sl_chess_move tmp[CH_MAX_MOVES];
    int n = sl_chess_pseudo(p, tmp, captures_only), legal = 0;
    for (int i = 0; i < n; i++)
    {
        struct sl_chess_undo u;
        int us = p->side;
        sl_chess_make(p, &tmp[i], &u);
        bool ok = !sl_chess_in_check(p, us);
        sl_chess_unmake(p, &tmp[i], &u);
        if (ok) out[legal++] = tmp[i];
    }
    return legal;
}

static inline uint64_t sl_chess_perft(struct sl_chess_pos *p, int depth)
{
    if (!depth) return 1;
    struct sl_chess_move m[CH_MAX_MOVES];
    int n = sl_chess_legal(p, m, false);
    if (depth == 1) return (uint64_t)n;
    uint64_t total = 0;
    for (int i = 0; i < n; i++)
    {
        struct sl_chess_undo u;
        sl_chess_make(p, &m[i], &u);
        total += sl_chess_perft(p, depth - 1);
        sl_chess_unmake(p, &m[i], &u);
    }
    return total;
}

/* The en passant square only counts when a pawn could take there: a
 * double step nobody can capture leaves the same position as any other
 * move, and the repetition rule must see them as one. */
static inline uint32_t sl_chess_hash(const struct sl_chess_pos *p)
{
    uint32_t h = 2166136261u;
    for (int sq = 0; sq < 128; sq++)
        if (CH_ONBOARD(sq)) h = (h ^ (uint8_t)p->board[sq]) * 16777619u;
    h = (h ^ (uint8_t)p->side) * 16777619u;
    h = (h ^ p->castle) * 16777619u;
    int ep = -1;
    if (p->ep >= 0)
        for (int d = -1; d <= 1; d += 2)
        {
            int from = p->ep + (p->side > 0 ? -16 : 16) + d;
            if (CH_ONBOARD(from) && p->board[from] == p->side * CH_P) ep = p->ep;
        }
    h = (h ^ (uint8_t)(ep + 1)) * 16777619u;
    return h;
}

static inline bool sl_chess_insufficient(const struct sl_chess_pos *p)
{
    int minors = 0, bishops_colour[2] = { 0, 0 }, knights = 0;
    for (int sq = 0; sq < 128; sq++)
    {
        if (!CH_ONBOARD(sq)) { sq += 7; continue; }
        int t = p->board[sq] > 0 ? p->board[sq] : -p->board[sq];
        if (!t || t == CH_K) continue;
        if (t == CH_P || t == CH_R || t == CH_Q) return false;
        minors++;
        if (t == CH_N) knights++;
        else bishops_colour[(CH_FILE(sq) + CH_RANK(sq)) & 1]++;
    }
    if (minors <= 1) return true;
    /* Only bishops, all on one colour, can never mate. */
    return !knights && (!bishops_colour[0] || !bishops_colour[1]);
}

/* Standard algebraic notation for mv, played from p (left unchanged):
 * "Nbd7", "exd8=Q+", "O-O-O#". out holds at least 8 bytes. */
static inline void sl_chess_san(struct sl_chess_pos *p, const struct sl_chess_move *mv, char *out)
{
    static const char names[] = "  NBRQK";
    int8_t pc = p->board[mv->from];
    int type = pc > 0 ? pc : -pc;
    char *w = out;
    struct sl_chess_move legal[CH_MAX_MOVES];
    int n = sl_chess_legal(p, legal, false);
    if (mv->flags & CHF_CASTLE)
    {
        const char *c = CH_FILE(mv->to) == 6 ? "O-O" : "O-O-O";
        while (*c) *w++ = *c++;
    }
    else
    {
        if (type != CH_P)
        {
            *w++ = names[type];
            bool other = false, same_file = false, same_rank = false;
            for (int i = 0; i < n; i++)
                if (legal[i].to == mv->to && legal[i].from != mv->from && p->board[legal[i].from] == pc)
                {
                    other = true;
                    if (CH_FILE(legal[i].from) == CH_FILE(mv->from)) same_file = true;
                    if (CH_RANK(legal[i].from) == CH_RANK(mv->from)) same_rank = true;
                }
            if (other && (!same_file || same_rank)) *w++ = (char)('a' + CH_FILE(mv->from));
            if (other && same_file) *w++ = (char)('1' + CH_RANK(mv->from));
        }
        else if (mv->flags & CHF_CAPTURE) *w++ = (char)('a' + CH_FILE(mv->from));
        if (mv->flags & CHF_CAPTURE) *w++ = 'x';
        *w++ = (char)('a' + CH_FILE(mv->to));
        *w++ = (char)('1' + CH_RANK(mv->to));
        if (mv->promo) { *w++ = '='; *w++ = names[mv->promo]; }
    }
    struct sl_chess_undo u;
    sl_chess_make(p, mv, &u);
    if (sl_chess_in_check(p, p->side))
    {
        struct sl_chess_move replies[CH_MAX_MOVES];
        *w++ = sl_chess_legal(p, replies, false) ? '+' : '#';
    }
    sl_chess_unmake(p, mv, &u);
    *w = 0;
}

/* ------------------------------------------------------------ opponent */
static const int16_t ch_value[7] = { 0, 100, 320, 330, 500, 900, 0 };
/* Piece-square tables from White's side, rank 0 first. */
static const int8_t ch_pst[7][64] = {
    { 0 },
    { 0,0,0,0,0,0,0,0, 5,10,10,-20,-20,10,10,5, 5,-5,-10,0,0,-10,-5,5, 0,0,0,20,20,0,0,0,
      5,5,10,25,25,10,5,5, 10,10,20,30,30,20,10,10, 50,50,50,50,50,50,50,50, 0,0,0,0,0,0,0,0 },
    { -50,-40,-30,-30,-30,-30,-40,-50, -40,-20,0,5,5,0,-20,-40, -30,5,10,15,15,10,5,-30, -30,0,15,20,20,15,0,-30,
      -30,5,15,20,20,15,5,-30, -30,0,10,15,15,10,0,-30, -40,-20,0,0,0,0,-20,-40, -50,-40,-30,-30,-30,-30,-40,-50 },
    { -20,-10,-10,-10,-10,-10,-10,-20, -10,5,0,0,0,0,5,-10, -10,10,10,10,10,10,10,-10, -10,0,10,10,10,10,0,-10,
      -10,5,5,10,10,5,5,-10, -10,0,5,10,10,5,0,-10, -10,0,0,0,0,0,0,-10, -20,-10,-10,-10,-10,-10,-10,-20 },
    { 0,0,0,5,5,0,0,0, -5,0,0,0,0,0,0,-5, -5,0,0,0,0,0,0,-5, -5,0,0,0,0,0,0,-5,
      -5,0,0,0,0,0,0,-5, -5,0,0,0,0,0,0,-5, 5,10,10,10,10,10,10,5, 0,0,0,0,0,0,0,0 },
    { -20,-10,-10,-5,-5,-10,-10,-20, -10,0,5,0,0,0,0,-10, -10,5,5,5,5,5,0,-10, 0,0,5,5,5,5,0,-5,
      -5,0,5,5,5,5,0,-5, -10,0,5,5,5,5,0,-10, -10,0,0,0,0,0,0,-10, -20,-10,-10,-5,-5,-10,-10,-20 },
    { 20,30,10,0,0,10,30,20, 20,20,0,0,0,0,20,20, -10,-20,-20,-20,-20,-20,-20,-10, -20,-30,-30,-40,-40,-30,-30,-20,
      -30,-40,-40,-50,-50,-40,-40,-30, -30,-40,-40,-50,-50,-40,-40,-30, -30,-40,-40,-50,-50,-40,-40,-30, -30,-40,-40,-50,-50,-40,-40,-30 },
};
/* In the endgame the king walks to the middle instead of hiding. */
static const int8_t ch_king_end[64] = {
    -50,-30,-30,-30,-30,-30,-30,-50, -30,-30,0,0,0,0,-30,-30, -30,-10,20,30,30,20,-10,-30, -30,-10,30,40,40,30,-10,-30,
    -30,-10,30,40,40,30,-10,-30, -30,-10,20,30,30,20,-10,-30, -30,-20,-10,0,0,-10,-20,-30, -50,-40,-30,-20,-20,-30,-40,-50 };
static const int8_t ch_passed[8] = { 0, 5, 10, 20, 35, 60, 100, 0 };

/* Material and placement, blended from middlegame to endgame as the
 * pieces come off, plus pawn structure and the bishop pair. From the
 * side to move's point of view. */
static inline int sl_chess_eval(const struct sl_chess_pos *p)
{
    int mid = 0, end = 0, phase = 0, bishops[2] = { 0, 0 };
    int8_t wlo[8], whi[8], blo[8], bhi[8], wn[8], bn[8];
    for (int f = 0; f < 8; f++) { wlo[f] = blo[f] = 8; whi[f] = bhi[f] = -1; wn[f] = bn[f] = 0; }
    for (int sq = 0; sq < 128; sq++)
    {
        if (!CH_ONBOARD(sq)) { sq += 7; continue; }
        int8_t pc = p->board[sq];
        if (!pc) continue;
        int t = pc > 0 ? pc : -pc, f = CH_FILE(sq), r = CH_RANK(sq);
        int idx = pc > 0 ? r * 8 + f : (7 - r) * 8 + f, sign = pc > 0 ? 1 : -1;
        if (t == CH_K)
        {
            mid += sign * ch_pst[CH_K][idx];
            end += sign * ch_king_end[idx];
            continue;
        }
        int v = ch_value[t] + ch_pst[t][idx];
        mid += sign * v; end += sign * v;
        phase += t == CH_N || t == CH_B ? 1 : t == CH_R ? 2 : t == CH_Q ? 4 : 0;
        if (t == CH_B) bishops[pc < 0]++;
        if (t == CH_P && pc > 0) { wn[f]++; if (r < wlo[f]) wlo[f] = (int8_t)r; if (r > whi[f]) whi[f] = (int8_t)r; }
        if (t == CH_P && pc < 0) { bn[f]++; if (r < blo[f]) blo[f] = (int8_t)r; if (r > bhi[f]) bhi[f] = (int8_t)r; }
    }
    for (int f = 0; f < 8; f++)
    {
        if (wn[f] > 1) { mid -= 12 * (wn[f] - 1); end -= 20 * (wn[f] - 1); }
        if (bn[f] > 1) { mid += 12 * (bn[f] - 1); end += 20 * (bn[f] - 1); }
        /* a front pawn no enemy pawn can meet on its way up */
        bool wpass = wn[f] > 0, bpass = bn[f] > 0;
        for (int g = f > 0 ? f - 1 : 0; g <= (f < 7 ? f + 1 : 7); g++)
        {
            if (wpass && bhi[g] > whi[f]) wpass = false;
            if (bpass && wlo[g] < blo[f]) bpass = false;
        }
        if (wpass) { mid += ch_passed[whi[f]] / 2; end += ch_passed[whi[f]]; }
        if (bpass) { mid -= ch_passed[7 - blo[f]] / 2; end -= ch_passed[7 - blo[f]]; }
    }
    if (bishops[0] >= 2) { mid += 30; end += 40; }
    if (bishops[1] >= 2) { mid -= 30; end -= 40; }
    if (phase > 24) phase = 24;
    return (mid * phase + end * (24 - phase)) / 24 * p->side;
}

/* Strength levels: how deep, how sloppy, and how long it may think. */
#define CH_LEVELS 5
static const struct { uint8_t depth; int16_t noise; uint32_t nodes, ms; } ch_levels[CH_LEVELS] = {
    { 1, 220, 3000, 350 },       /* Beginner: one move ahead, often careless */
    { 2, 60, 15000, 600 },       /* Easy */
    { 3, 16, 60000, 900 },       /* Medium */
    { 6, 0, 600000, 1600 },      /* Hard */
    { 40, 0, 8000000, 3000 },    /* Master: as deep as the clock allows */
};
static inline uint32_t sl_chess_level_ms(int level)
{
    return ch_levels[level < 0 ? 0 : level >= CH_LEVELS ? CH_LEVELS - 1 : level].ms;
}

#define CH_PLY_MAX 64
#define CH_TT_BITS 14
enum { CH_TT_EXACT = 1, CH_TT_LOWER, CH_TT_UPPER };
struct sl_chess_tt {
    uint32_t key;
    int16_t score;
    int8_t depth;
    uint8_t flag;
    struct sl_chess_move move;
};
/* One table: there is only ever one search at a time. */
static struct sl_chess_tt ch_tt[1 << CH_TT_BITS];

struct sl_chess_search {
    uint32_t nodes, node_limit, rng;
    uint32_t (*now_ms)(void);   /* optional clock for a time budget */
    uint32_t deadline, next_check;
    bool stopped;
    const volatile bool *abort; /* optional: set to drop the search */
    const uint32_t *history;    /* earlier positions of the game, oldest first */
    int nhistory;
    uint32_t path[CH_PLY_MAX + 2];
    struct sl_chess_move killer[CH_PLY_MAX][2];
    int16_t hist[64][64];
};

static inline int ch_idx(int sq) { return CH_RANK(sq) * 8 + CH_FILE(sq); }
static inline bool ch_same(const struct sl_chess_move *a, const struct sl_chess_move *b)
{
    return a->from == b->from && a->to == b->to && a->promo == b->promo;
}

static inline bool ch_out_of_budget(struct sl_chess_search *s)
{
    if (s->nodes >= s->node_limit) return true;
    /* the clock is cheap but not free: look at it about every 1024 nodes */
    if (s->nodes < s->next_check) return false;
    s->next_check = s->nodes + 1024;
    if (s->abort && *s->abort) return true;
    return s->now_ms && (int32_t)(s->now_ms() - s->deadline) >= 0;
}

static inline int ch_order_key(const struct sl_chess_pos *p, const struct sl_chess_move *m,
                               const struct sl_chess_search *s, int ply, const struct sl_chess_move *best)
{
    if (best && ch_same(m, best)) return 1 << 30;
    int key = 0;
    if (m->flags & CHF_CAPTURE)
    {
        int victim = p->board[m->to] ? (p->board[m->to] > 0 ? p->board[m->to] : -p->board[m->to]) : CH_P;
        int attacker = p->board[m->from] > 0 ? p->board[m->from] : -p->board[m->from];
        key = (1 << 24) + ch_value[victim] * 16 - ch_value[attacker] / 10;
    }
    if (m->promo) key += (1 << 24) + ch_value[m->promo];
    if (!key && s && ply < CH_PLY_MAX)
    {
        if (ch_same(m, &s->killer[ply][0])) return (1 << 23) + 2;
        if (ch_same(m, &s->killer[ply][1])) return (1 << 23) + 1;
        key = s->hist[ch_idx(m->from)][ch_idx(m->to)];
    }
    return key;
}

static inline void ch_sort(const struct sl_chess_pos *p, struct sl_chess_move *m, int n,
                           const struct sl_chess_search *s, int ply, const struct sl_chess_move *best)
{
    int keys[CH_MAX_MOVES];
    for (int i = 0; i < n; i++) keys[i] = ch_order_key(p, &m[i], s, ply, best);
    for (int i = 1; i < n; i++)
    {
        struct sl_chess_move mv = m[i]; int k = keys[i], j = i - 1;
        while (j >= 0 && keys[j] < k) { m[j + 1] = m[j]; keys[j + 1] = keys[j]; j--; }
        m[j + 1] = mv; keys[j + 1] = k;
    }
}

static inline int ch_quiesce(struct sl_chess_pos *p, int alpha, int beta, struct sl_chess_search *s, int depth)
{
    s->nodes++;
    int stand = sl_chess_eval(p);
    if (stand >= beta || depth <= 0) return stand;
    if (stand > alpha) alpha = stand;
    struct sl_chess_move m[CH_MAX_MOVES];
    int n = sl_chess_legal(p, m, true);
    ch_sort(p, m, n, NULL, 0, NULL);
    for (int i = 0; i < n; i++)
    {
        struct sl_chess_undo u;
        sl_chess_make(p, &m[i], &u);
        int score = -ch_quiesce(p, -beta, -alpha, s, depth - 1);
        sl_chess_unmake(p, &m[i], &u);
        if (score >= beta) return score;
        if (score > alpha) alpha = score;
    }
    return alpha;
}

/* The same position earlier in the game or in this line counts as a
 * draw: the search neither walks into one when winning nor misses one
 * that saves it. */
static inline bool ch_repeats(const struct sl_chess_pos *p, const struct sl_chess_search *s, int ply, uint32_t h)
{
    for (int d = 2; d <= p->halfmove; d += 2)
    {
        uint32_t seen;
        if (d <= ply) seen = s->path[ply - d];
        else
        {
            int k = s->nhistory - (d - ply);
            if (k < 0) break;
            seen = s->history[k];
        }
        if (seen == h) return true;
    }
    return false;
}

static inline struct sl_chess_tt *ch_tt_slot(uint32_t h) { return &ch_tt[(h * 2654435761u) >> (32 - CH_TT_BITS)]; }
/* mate scores are stored as distance from this node, not from the root */
static inline int ch_tt_in(int score, int ply) { return score > CH_MATE - 200 ? score + ply : score < -CH_MATE + 200 ? score - ply : score; }
static inline int ch_tt_out(int score, int ply) { return score > CH_MATE - 200 ? score - ply : score < -CH_MATE + 200 ? score + ply : score; }

static inline bool ch_has_pieces(const struct sl_chess_pos *p, int side)
{
    for (int sq = 0; sq < 128; sq++)
    {
        if (!CH_ONBOARD(sq)) { sq += 7; continue; }
        int8_t pc = (int8_t)(p->board[sq] * side);
        if (pc > CH_P && pc < CH_K) return true;
    }
    return false;
}

static inline int ch_negamax(struct sl_chess_pos *p, int depth, int alpha, int beta, int ply,
                             struct sl_chess_search *s, bool null_ok)
{
    if (s->stopped || ch_out_of_budget(s)) { s->stopped = true; return 0; }
    uint32_t h = sl_chess_hash(p);
    s->path[ply] = h;
    if (ch_repeats(p, s, ply, h) || p->halfmove >= 100) return 0;
    bool check = sl_chess_in_check(p, p->side);
    if (check && ply < CH_PLY_MAX - 16) depth++;          /* never stop the line in check */
    if (depth <= 0 || ply >= CH_PLY_MAX - 2) return ch_quiesce(p, alpha, beta, s, 6);
    s->nodes++;
    struct sl_chess_tt *e = ch_tt_slot(h);
    struct sl_chess_move tt_move = { 0, 0, 0, 0 };
    bool have_tt = e->key == h && e->flag;
    if (have_tt)
    {
        tt_move = e->move;
        int sc = ch_tt_out(e->score, ply);
        if (e->depth >= depth &&
            (e->flag == CH_TT_EXACT || (e->flag == CH_TT_LOWER && sc >= beta) || (e->flag == CH_TT_UPPER && sc <= alpha)))
            return sc;
    }
    /* Null move: if passing still keeps us above beta, a real move will
     * too. Not in check, and not with only pawns left (zugzwang). */
    if (null_ok && depth >= 3 && !check && beta < CH_MATE - 200 && ch_has_pieces(p, p->side) &&
        sl_chess_eval(p) >= beta)
    {
        int16_t ep = p->ep;
        p->ep = -1; p->side = (int8_t)-p->side;
        int score = -ch_negamax(p, depth - 3, -beta, -beta + 1, ply + 1, s, false);
        p->side = (int8_t)-p->side; p->ep = ep;
        if (s->stopped) return 0;
        if (score >= beta) return score;
    }
    struct sl_chess_move m[CH_MAX_MOVES];
    int n = sl_chess_legal(p, m, false);
    if (!n) return check ? -CH_MATE + ply : 0;
    ch_sort(p, m, n, s, ply, have_tt ? &tt_move : NULL);
    int best = -CH_MATE - 1, alpha0 = alpha;
    struct sl_chess_move best_move = m[0];
    for (int i = 0; i < n; i++)
    {
        struct sl_chess_undo u;
        sl_chess_make(p, &m[i], &u);
        int score = -ch_negamax(p, depth - 1, -beta, -alpha, ply + 1, s, true);
        sl_chess_unmake(p, &m[i], &u);
        if (s->stopped) return best > -CH_MATE ? best : 0;
        if (score > best) { best = score; best_move = m[i]; }
        if (score > alpha) alpha = score;
        if (alpha >= beta)
        {
            if (!(m[i].flags & CHF_CAPTURE) && !m[i].promo)
            {
                if (!ch_same(&m[i], &s->killer[ply][0])) { s->killer[ply][1] = s->killer[ply][0]; s->killer[ply][0] = m[i]; }
                int16_t *hv = &s->hist[ch_idx(m[i].from)][ch_idx(m[i].to)];
                *hv = (int16_t)(*hv + depth * depth > 30000 ? 30000 : *hv + depth * depth);
            }
            break;
        }
    }
    e->key = h;
    e->score = (int16_t)ch_tt_in(best, ply);
    e->depth = (int8_t)depth;
    e->flag = best <= alpha0 ? CH_TT_UPPER : best >= beta ? CH_TT_LOWER : CH_TT_EXACT;
    e->move = best_move;
    return best;
}

static inline uint32_t ch_rand(uint32_t *state)
{
    uint32_t x = *state ? *state : 0x9e3779b9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *state = x;
}

/* A few first moves, so the strong levels do not open the same way
 * every game. Only from the starting position and the reply to it. */
static inline bool ch_book(const struct sl_chess_pos *p, const struct sl_chess_move *m, int n,
                           uint32_t *rng, struct sl_chess_move *out)
{
    static const char *const white[] = { "e2e4", "d2d4", "c2c4", "g1f3", "e2e4", "d2d4" };
    static const char *const after_e4[] = { "e7e5", "c7c5", "e7e6", "c7c6" };
    static const char *const after_d4[] = { "d7d5", "g8f6", "e7e6" };
    static const char *const after_other[] = { "g8f6", "d7d5", "e7e5" };
    struct sl_chess_pos start;
    sl_chess_init(&start);
    const char *const *list = NULL;
    int count = 0;
    if (p->side > 0 && !memcmp(p->board, start.board, sizeof start.board)) { list = white; count = 6; }
    else if (p->side < 0 && p->fullmove == 1 && p->castle == 15)
    {
        int diff = 0, pawn_to = -1;
        for (int sq = 0; sq < 128; sq++)
            if (CH_ONBOARD(sq) && p->board[sq] != start.board[sq]) { diff++; if (p->board[sq] == CH_P) pawn_to = sq; }
        if (diff != 2) return false;
        if (pawn_to == CH_SQ(4, 3)) { list = after_e4; count = 4; }
        else if (pawn_to == CH_SQ(3, 3)) { list = after_d4; count = 3; }
        else { list = after_other; count = 3; }
    }
    if (!list) return false;
    const char *u = list[ch_rand(rng) % (uint32_t)count];
    for (int i = 0; i < n; i++)
        if (m[i].from == CH_SQ(u[0] - 'a', u[1] - '1') && m[i].to == CH_SQ(u[2] - 'a', u[3] - '1'))
        { *out = m[i]; return true; }
    return false;
}

/* A request to the opponent. Everything but level and seed may be left 0. */
struct sl_chess_ask {
    int level;                  /* 0 Beginner .. CH_LEVELS - 1 Master */
    uint32_t seed;
    uint32_t (*now_ms)(void);   /* with budget_ms: stop on the clock */
    uint32_t budget_ms;
    const uint32_t *history;    /* hashes of the positions before this one */
    int nhistory;
    const volatile bool *abort;
    int depth, score;           /* out: last finished depth and its score */
};

/* Iterative deepening: plays the best move of the last depth that
 * finished before the clock, the node limit or abort. Returns false when
 * there is no move. */
static inline bool sl_chess_think_ask(struct sl_chess_pos *p, struct sl_chess_ask *a, struct sl_chess_move *out)
{
    struct sl_chess_move m[CH_MAX_MOVES];
    int n = sl_chess_legal(p, m, false);
    if (!n) return false;
    int level = a->level < 0 ? 0 : a->level >= CH_LEVELS ? CH_LEVELS - 1 : a->level;
    static struct sl_chess_search s;      /* large: kept off the thread's stack */
    memset(&s, 0, sizeof s);
    s.node_limit = ch_levels[level].nodes; s.rng = a->seed | 1u;
    s.now_ms = a->now_ms; s.abort = a->abort;
    s.deadline = a->now_ms ? a->now_ms() + a->budget_ms : 0;
    s.next_check = 1024;
    s.history = a->history; s.nhistory = a->nhistory;
    s.path[0] = sl_chess_hash(p);
    int noise = ch_levels[level].noise;
    if (ch_book(p, m, n, &s.rng, out)) { a->depth = 0; return true; }
    ch_sort(p, m, n, &s, 0, NULL);
    int best_index = 0;
    a->depth = 0; a->score = 0;
    for (int depth = 1; depth <= ch_levels[level].depth; depth++)
    {
        int alpha = -CH_MATE - 1, iter_best = -1, iter_score = -CH_MATE - 1;
        for (int i = 0; i < n; i++)
        {
            struct sl_chess_undo u;
            sl_chess_make(p, &m[i], &u);
            int score = -ch_negamax(p, depth - 1, -CH_MATE - 1, -alpha, 1, &s, true);
            sl_chess_unmake(p, &m[i], &u);
            if (s.stopped) break;
            if (noise && score < CH_MATE - 200 && score > -CH_MATE + 200)
                score += (int)(ch_rand(&s.rng) % (uint32_t)(2 * noise + 1)) - noise;
            if (score > iter_score) { iter_score = score; iter_best = i; }
            /* with noise every move needs its true score, not a bound:
             * a bound plus noise could beat the real best move */
            if (score > alpha && !noise) alpha = score;
        }
        if (iter_best >= 0 && (!s.stopped || depth == 1)) { best_index = iter_best; a->depth = depth; a->score = iter_score; }
        if (s.stopped) break;
        /* Search the previous best first next time. */
        struct sl_chess_move keep = m[best_index];
        memmove(&m[1], &m[0], sizeof m[0] * (size_t)best_index);
        m[0] = keep; best_index = 0;
        if (iter_score >= CH_MATE - 200) break;
    }
    *out = m[best_index];
    return true;
}

/* level 0 Beginner .. 4 Master. With now_ms, the search also stops
 * budget_ms after it starts. */
static inline bool sl_chess_think_timed(struct sl_chess_pos *p, int level, uint32_t seed,
                                        struct sl_chess_move *out,
                                        uint32_t (*now_ms)(void), uint32_t budget_ms)
{
    struct sl_chess_ask a = { .level = level, .seed = seed, .now_ms = now_ms, .budget_ms = budget_ms };
    return sl_chess_think_ask(p, &a, out);
}

static inline bool sl_chess_think(struct sl_chess_pos *p, int level, uint32_t seed, struct sl_chess_move *out)
{
    return sl_chess_think_timed(p, level, seed, out, NULL, 0);
}

/* ---------------------------------------------------------- game record */
#define SL_CHESS_MAGIC 0x32534843u    /* CHS2: five levels */
#define SL_CHESS_MAGIC_V1 0x31534843u /* CHS1: Easy, Medium, Hard */
struct sl_chess_game {
    uint32_t magic;
    struct sl_chess_pos pos;
    uint8_t mode;          /* 0 two players, 1 against the computer */
    uint8_t level;         /* 0 Beginner .. CH_LEVELS - 1 */
    int8_t human;          /* side the person plays against the computer */
    uint8_t flip;          /* two-player: turn the board each move */
    uint16_t plies;
    struct sl_chess_move moves[CH_MAX_PLIES];
    struct sl_chess_undo undo[CH_MAX_PLIES];
    uint32_t hashes[CH_MAX_PLIES + 1];
    uint32_t checksum;
};

static inline uint32_t sl_chess_game_checksum(const struct sl_chess_game *g)
{
    const uint8_t *b = (const uint8_t *)g;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof *g - sizeof g->checksum; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

static inline void sl_chess_new(struct sl_chess_game *g, int mode, int level, int human)
{
    memset(g, 0, sizeof *g);
    g->magic = SL_CHESS_MAGIC;
    sl_chess_init(&g->pos);
    g->mode = (uint8_t)mode; g->level = (uint8_t)level;
    g->human = (int8_t)(human < 0 ? -1 : 1); g->flip = 1;
    g->hashes[0] = sl_chess_hash(&g->pos);
    g->checksum = sl_chess_game_checksum(g);
}

/* Fields the move code uses as board indexes. A file that passes the
 * checksum but holds a bad king square or move would read outside board[]. */
static inline bool ch_game_sane(const struct sl_chess_game *g)
{
    const struct sl_chess_pos *p = &g->pos;
    for (int sq = 0; sq < 128; sq++)
        if (CH_ONBOARD(sq) && (p->board[sq] < -CH_K || p->board[sq] > CH_K)) return false;
    for (int i = 0; i < 2; i++)
        if (p->king[i] > 127 || !CH_ONBOARD(p->king[i]) ||
            p->board[p->king[i]] != (i ? -CH_K : CH_K)) return false;
    if (p->ep != -1 && (p->ep < 0 || p->ep > 127 || !CH_ONBOARD(p->ep))) return false;
    for (int i = 0; i < g->plies; i++)
    {
        const struct sl_chess_move *m = &g->moves[i];
        if (m->from > 127 || m->to > 127 || !CH_ONBOARD(m->from) || !CH_ONBOARD(m->to) ||
            m->promo < 0 || m->promo > CH_K) return false;
    }
    return true;
}

static inline bool sl_chess_game_valid(const struct sl_chess_game *g)
{
    return g->magic == SL_CHESS_MAGIC && g->plies <= CH_MAX_PLIES && g->mode <= 1 &&
           g->level < CH_LEVELS && (g->pos.side == 1 || g->pos.side == -1) &&
           g->checksum == sl_chess_game_checksum(g) && ch_game_sane(g);
}

/* A game saved before there were five levels: same layout, and its
 * Easy, Medium and Hard are the new Easy, Medium and Hard. */
static inline bool sl_chess_game_upgrade(struct sl_chess_game *g)
{
    if (g->magic != SL_CHESS_MAGIC_V1 || g->checksum != sl_chess_game_checksum(g) || g->level > 2) return false;
    g->magic = SL_CHESS_MAGIC;
    g->level++;
    g->checksum = sl_chess_game_checksum(g);
    return sl_chess_game_valid(g);
}

static inline bool sl_chess_play(struct sl_chess_game *g, const struct sl_chess_move *mv)
{
    if (g->plies >= CH_MAX_PLIES) return false;
    struct sl_chess_move legal[CH_MAX_MOVES];
    int n = sl_chess_legal(&g->pos, legal, false);
    for (int i = 0; i < n; i++)
        if (legal[i].from == mv->from && legal[i].to == mv->to &&
            (legal[i].promo == mv->promo || (!mv->promo && legal[i].promo == CH_Q)))
        {
            g->moves[g->plies] = legal[i];
            sl_chess_make(&g->pos, &legal[i], &g->undo[g->plies]);
            g->plies++;
            g->hashes[g->plies] = sl_chess_hash(&g->pos);
            g->checksum = sl_chess_game_checksum(g);
            return true;
        }
    return false;
}

static inline bool sl_chess_takeback(struct sl_chess_game *g)
{
    if (!g->plies) return false;
    g->plies--;
    sl_chess_unmake(&g->pos, &g->moves[g->plies], &g->undo[g->plies]);
    g->checksum = sl_chess_game_checksum(g);
    return true;
}

static inline enum sl_chess_status sl_chess_game_status(struct sl_chess_game *g)
{
    struct sl_chess_move m[CH_MAX_MOVES];
    int n = sl_chess_legal(&g->pos, m, false);
    if (!n) return sl_chess_in_check(&g->pos, g->pos.side) ? CH_CHECKMATE : CH_STALEMATE;
    if (g->pos.halfmove >= 100) return CH_DRAW_FIFTY;
    /* sl_chess_play refuses moves past the record's size; without this the
     * game stayed "playing" and the computer re-searched forever */
    if (g->plies >= CH_MAX_PLIES) return CH_DRAW_LENGTH;
    if (sl_chess_insufficient(&g->pos)) return CH_DRAW_MATERIAL;
    int seen = 0;
    for (int i = (int)g->plies; i >= 0 && (int)g->plies - i <= g->pos.halfmove; i -= 2)
        if (g->hashes[i] == g->hashes[g->plies]) seen++;
    return seen >= 3 ? CH_DRAW_REPETITION : CH_PLAYING;
}

#endif
