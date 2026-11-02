#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../sloppy_chess_state.h"

static uint64_t perft_fen(const char *fen, int depth)
{
    struct sl_chess_pos p;
    assert(sl_chess_from_fen(&p, fen));
    return sl_chess_perft(&p, depth);
}

static struct sl_chess_move mv(const char *uci)
{
    struct sl_chess_move m = { (uint8_t)CH_SQ(uci[0] - 'a', uci[1] - '1'),
                               (uint8_t)CH_SQ(uci[2] - 'a', uci[3] - '1'), 0, 0 };
    if (uci[4]) m.promo = uci[4] == 'n' ? CH_N : uci[4] == 'b' ? CH_B : uci[4] == 'r' ? CH_R : CH_Q;
    return m;
}

static uint32_t fake_clock_ms;
static int fake_clock_calls;
static uint32_t fake_clock(void) { fake_clock_calls++; uint32_t t = fake_clock_ms; fake_clock_ms += 50; return t; }

int main(void)
{
    /* Reference perft counts (chessprogramming.org). */
    struct sl_chess_pos start;
    sl_chess_init(&start);
    assert(sl_chess_perft(&start, 1) == 20);
    assert(sl_chess_perft(&start, 2) == 400);
    assert(sl_chess_perft(&start, 3) == 8902);
    assert(sl_chess_perft(&start, 4) == 197281);
    const char *kiwipete = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq -";
    assert(perft_fen(kiwipete, 1) == 48);
    assert(perft_fen(kiwipete, 2) == 2039);
    assert(perft_fen(kiwipete, 3) == 97862);
    const char *pos3 = "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - -";
    assert(perft_fen(pos3, 1) == 14);
    assert(perft_fen(pos3, 2) == 191);
    assert(perft_fen(pos3, 3) == 2812);
    assert(perft_fen(pos3, 4) == 43238);
    const char *pos4 = "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq -";
    assert(perft_fen(pos4, 1) == 6);
    assert(perft_fen(pos4, 2) == 264);
    assert(perft_fen(pos4, 3) == 9467);
    const char *pos5 = "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ -";
    assert(perft_fen(pos5, 1) == 44);
    assert(perft_fen(pos5, 2) == 1486);
    assert(perft_fen(pos5, 3) == 62379);

    /* Fool's mate, then take back and check the record stays valid. */
    struct sl_chess_game g;
    sl_chess_new(&g, 0, 1, 1);
    assert(sl_chess_game_valid(&g));
    const char *fool[] = { "f2f3", "e7e5", "g2g4", "d8h4" };
    for (int i = 0; i < 4; i++) { struct sl_chess_move m = mv(fool[i]); assert(sl_chess_play(&g, &m)); }
    assert(sl_chess_game_status(&g) == CH_CHECKMATE && g.pos.side == 1);
    struct sl_chess_move illegal = mv("e2e4");
    assert(!sl_chess_play(&g, &illegal));
    assert(sl_chess_takeback(&g) && sl_chess_game_status(&g) == CH_PLAYING && sl_chess_game_valid(&g));

    /* Castling, en passant and promotion through the game record. */
    sl_chess_new(&g, 0, 1, 1);
    const char *line[] = { "e2e4", "a7a6", "e4e5", "d7d5", "e5d6", "c7d6", "g1f3", "b7b5",
                           "f1e2", "b5b4", "e1g1", "b4b3", "a2b3" };
    for (int i = 0; i < 13; i++) { struct sl_chess_move m = mv(line[i]); assert(sl_chess_play(&g, &m)); }
    assert(g.pos.board[CH_SQ(6, 0)] == CH_K && g.pos.board[CH_SQ(5, 0)] == CH_R);
    assert(g.pos.board[CH_SQ(3, 4)] == 0 && g.moves[4].flags & CHF_EP);
    struct sl_chess_game promo;
    sl_chess_new(&promo, 0, 1, 1);
    assert(sl_chess_from_fen(&promo.pos, "8/P6k/8/8/8/8/8/K7 w - -"));
    struct sl_chess_move up = mv("a7a8"); /* no piece chosen: queen */
    assert(sl_chess_play(&promo, &up) && promo.pos.board[CH_SQ(0, 7)] == CH_Q);
    sl_chess_takeback(&promo);
    struct sl_chess_move knight = mv("a7a8n");
    assert(sl_chess_play(&promo, &knight) && promo.pos.board[CH_SQ(0, 7)] == CH_N);

    /* Stalemate and insufficient material. */
    struct sl_chess_game draw;
    sl_chess_new(&draw, 0, 1, 1);
    assert(sl_chess_from_fen(&draw.pos, "7k/5Q2/6K1/8/8/8/8/8 b - -"));
    assert(sl_chess_game_status(&draw) == CH_STALEMATE);
    assert(sl_chess_from_fen(&draw.pos, "8/8/4k3/8/8/2B5/4K3/8 w - -"));
    assert(sl_chess_game_status(&draw) == CH_DRAW_MATERIAL);
    assert(sl_chess_from_fen(&draw.pos, "8/8/4k3/8/8/2N5/4KN2/8 w - -"));
    assert(sl_chess_game_status(&draw) == CH_PLAYING);

    /* Threefold repetition by shuffling knights. */
    sl_chess_new(&draw, 0, 1, 1);
    const char *shuffle[] = { "g1f3", "g8f6", "f3g1", "f6g8", "g1f3", "g8f6", "f3g1", "f6g8" };
    for (int i = 0; i < 8; i++) { struct sl_chess_move m = mv(shuffle[i]); assert(sl_chess_play(&draw, &m)); }
    assert(sl_chess_game_status(&draw) == CH_DRAW_REPETITION);

    /* The opponent finds mate in one at every level and never plays illegally. */
    for (int level = 0; level < CH_LEVELS; level++)
    {
        struct sl_chess_pos p;
        assert(sl_chess_from_fen(&p, "6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - -"));
        struct sl_chess_move best;
        assert(sl_chess_think(&p, level, 1234, &best));
        struct sl_chess_undo u;
        sl_chess_make(&p, &best, &u);
        struct sl_chess_move replies[CH_MAX_MOVES];
        if (level) assert(!sl_chess_legal(&p, replies, false) && sl_chess_in_check(&p, p.side));
    }
    /* Every level but Beginner takes a hanging queen (the root noise once
     * let a cut-off bound beat it). */
    struct sl_chess_pos p;
    assert(sl_chess_from_fen(&p, "4k3/8/8/3q4/8/8/3R4/4K3 w - -"));
    struct sl_chess_move grab;
    for (int level = 1; level < CH_LEVELS; level++)
        assert(sl_chess_think(&p, level, 99, &grab) && grab.to == CH_SQ(3, 4));
    /* Hard and Master find a mate in two (a quiet rook lift, then mate). */
    for (int level = 3; level < CH_LEVELS; level++)
    {
        struct sl_chess_game mg;
        sl_chess_new(&mg, 1, level, 1);
        assert(sl_chess_from_fen(&mg.pos, "7k/8/5K2/8/8/8/8/R7 w - -"));
        mg.pos.fullmove = 30;
        struct sl_chess_move first, reply, last;
        assert(sl_chess_think(&mg.pos, level, 5, &first) && sl_chess_play(&mg, &first));
        assert(sl_chess_think(&mg.pos, 0, 5, &reply) && sl_chess_play(&mg, &reply));
        assert(sl_chess_think(&mg.pos, level, 5, &last) && sl_chess_play(&mg, &last));
        assert(sl_chess_game_status(&mg) == CH_CHECKMATE);
    }
    /* Notation: pieces, captures, disambiguation, castling, promotion, check and mate. */
    {
        struct sl_chess_pos sp;
        char san[8];
        struct sl_chess_move sm;
        assert(sl_chess_from_fen(&sp, "r3k2r/8/8/8/8/2N3N1/8/R3K2R w KQkq -"));
        sm = mv("c3e4"); sm.flags = 0; sl_chess_san(&sp, &sm, san); assert(!strcmp(san, "Nce4"));
        sm = mv("a1a8"); sm.flags = CHF_CAPTURE; sl_chess_san(&sp, &sm, san); assert(!strcmp(san, "Rxa8+"));
        sm = mv("e1g1"); sm.flags = CHF_CASTLE; sl_chess_san(&sp, &sm, san); assert(!strcmp(san, "O-O"));
        sm = mv("e1c1"); sm.flags = CHF_CASTLE; sl_chess_san(&sp, &sm, san); assert(!strcmp(san, "O-O-O"));
        assert(sl_chess_from_fen(&sp, "3r3k/4P1pp/8/8/8/8/8/R3K3 w - -"));
        sm = mv("e7d8q"); sm.flags = CHF_CAPTURE; sl_chess_san(&sp, &sm, san); assert(!strcmp(san, "exd8=Q#"));
        assert(sl_chess_from_fen(&sp, "7k/8/8/8/8/8/8/R3R2K w - -"));
        sm = mv("a1c1"); sl_chess_san(&sp, &sm, san); assert(!strcmp(san, "Rac1"));
        assert(sl_chess_from_fen(&sp, "7k/8/8/8/R7/8/8/R6K w - -"));
        sm = mv("a1a2"); sl_chess_san(&sp, &sm, san); assert(!strcmp(san, "R1a2"));
        assert(sl_chess_from_fen(&sp, "6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - -"));
        sm = mv("d1d8"); sl_chess_san(&sp, &sm, san); assert(!strcmp(san, "Rd8#"));
        /* the record keeps every move playable back in notation */
        sl_chess_new(&g, 0, 2, 1);
        const char *ruy[] = { "e2e4", "e7e5", "g1f3", "b8c6", "f1b5" }, *want[] = { "e4", "e5", "Nf3", "Nc6", "Bb5" };
        for (int i = 0; i < 5; i++)
        {
            struct sl_chess_move m = mv(ruy[i]);
            sl_chess_san(&g.pos, &m, san);
            assert(sl_chess_play(&g, &m) && !strcmp(san, want[i]));
        }
    }
    /* A double step nobody can take en passant is the same position for
     * the repetition rule; one that can be taken is not. */
    {
        struct sl_chess_pos a, b;
        assert(sl_chess_from_fen(&a, "4k3/8/8/8/4P3/8/8/4K3 b - e3"));
        assert(sl_chess_from_fen(&b, "4k3/8/8/8/4P3/8/8/4K3 b - -"));
        assert(sl_chess_hash(&a) == sl_chess_hash(&b));
        assert(sl_chess_from_fen(&a, "4k3/8/8/8/3pP3/8/8/4K3 b - e3"));
        assert(sl_chess_from_fen(&b, "4k3/8/8/8/3pP3/8/8/4K3 b - -"));
        assert(sl_chess_hash(&a) != sl_chess_hash(&b));
    }
    /* A save from before the five levels is kept, its level carried over. */
    {
        struct sl_chess_game old;
        sl_chess_new(&old, 1, 2, -1);
        old.magic = SL_CHESS_MAGIC_V1; old.level = 2;   /* the old Hard */
        old.checksum = sl_chess_game_checksum(&old);
        assert(!sl_chess_game_valid(&old) && sl_chess_game_upgrade(&old));
        assert(sl_chess_game_valid(&old) && old.level == 3 && old.human == -1);
        old.magic = 0x12345678; assert(!sl_chess_game_upgrade(&old));
    }
    /* Abort drops a long search at once; the move is still legal. */
    {
        struct sl_chess_pos ap;
        assert(sl_chess_from_fen(&ap, "r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq -"));
        static volatile bool stop = true;
        struct sl_chess_ask ask = { .level = 4, .seed = 3, .abort = &stop };
        struct sl_chess_move am;
        assert(sl_chess_think_ask(&ap, &ask, &am) && ask.depth <= 2);
        struct sl_chess_game check;
        sl_chess_new(&check, 0, 0, 1);
        check.pos = ap;
        assert(sl_chess_play(&check, &am));
    }
    /* The strong levels open in more than one way. */
    {
        struct sl_chess_pos bp;
        sl_chess_init(&bp);
        int seen = 0;
        for (uint32_t seed = 1; seed < 40; seed++)
        {
            struct sl_chess_move bm;
            assert(sl_chess_think(&bp, 4, seed * 2654435761u, &bm));
            seen |= 1 << (CH_FILE(bm.from) + 8 * (CH_RANK(bm.to) - 2));
        }
        assert(__builtin_popcount(seen) >= 3);
    }
    /* A full self-play game stays legal and ends. */
    sl_chess_new(&g, 1, 1, 1);
    int moves = 0;
    while (sl_chess_game_status(&g) == CH_PLAYING && moves < 160)
    {
        struct sl_chess_move m;
        assert(sl_chess_think(&g.pos, moves % 2 ? 0 : 1, (uint32_t)moves * 7919u + 1, &m));
        assert(sl_chess_play(&g, &m));
        moves++;
    }
    assert(sl_chess_game_valid(&g));
    /* A time budget stops a hard search early and still returns a legal
     * move; the mate in one is found by depth 1. */
    {
        struct sl_chess_pos tp;
        struct sl_chess_move tm;
        assert(sl_chess_from_fen(&tp, "6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - -"));
        fake_clock_ms = 0; fake_clock_calls = 0;
        assert(sl_chess_think_timed(&tp, 3, 1234, &tm, fake_clock, 100));
        assert(fake_clock_calls <= 3 && tm.to == CH_SQ(3, 7));
        assert(sl_chess_from_fen(&tp, "r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq -"));
        fake_clock_ms = 0; fake_clock_calls = 0;
        assert(sl_chess_think_timed(&tp, 4, 1234, &tm, fake_clock, 200) && fake_clock_calls <= 6);
    }
    /* At the move-record limit the game is a draw, not "playing" forever. */
    {
        struct sl_chess_game lg;
        sl_chess_new(&lg, 1, 2, 1);
        lg.plies = CH_MAX_PLIES;
        assert(sl_chess_game_status(&lg) == CH_DRAW_LENGTH);
        /* a save with a good checksum but a bad king square is rejected */
        sl_chess_new(&lg, 1, 2, 1);
        assert(sl_chess_game_valid(&lg));
        lg.pos.king[1] = 0x78;   /* off the board */
        lg.checksum = sl_chess_game_checksum(&lg);
        assert(!sl_chess_game_valid(&lg));
    }
    printf("GabagoolOS chess perft, rules, draws and opponent tests passed (%d self-play plies)\n", moves);
    return 0;
}
