/* Sound effects for the GabagoolOS apps and games.
 *
 * Every sound is a few synthesised notes, not a sample: the beep channel
 * takes raw PCM with no decoder behind it, and a table of notes costs a few
 * bytes where a WAV would cost a card read on every tap. Notes are rendered
 * a chunk at a time from the mixer's callback, so a long sound needs no big
 * buffer. The channel is the one keyclicks use, and the music steps aside
 * for a sound when Sound > audio prioritisation is on (beep_duck).
 *
 * Settings > Sounds sets their volume with a slider (sloppy_sfx_volume);
 * it never touches the music. */
#include <math.h>
#include "pcm_mixer.h"
static int sfx_volume_pct(void);

enum sfx_wave { W_SINE, W_TRI, W_SQUARE, W_NOISE, W_BELL };
enum sfx_env  { E_PLUCK, E_HOLD, E_SWELL };

/* One note: when it starts and how long it lasts (ms), the pitch it glides
 * from and to (Hz; for noise, how bright it is), its wave, its envelope and
 * its level out of 100. */
struct sfx_note { uint16_t at, dur, f0, f1; uint8_t wave, env, vol; };
#define N(at, dur, f0, f1, wave, env, vol) { at, dur, f0, f1, wave, env, vol }
#define SFX_NOTES 8

/* How much a sound matters: a tick never cuts off a fanfare, and the
 * interface's own sounds give way to everything. */
enum { P_UI, P_TICK, P_NORMAL, P_RESULT };

enum sfx_id {
    SFX_TAP, SFX_ERROR, SFX_WIN, SFX_LOSE, SFX_DRAW, SFX_START,
    SFX_COIN_FLIP, SFX_COIN_LAND, SFX_COIN_HEADS, SFX_COIN_TAILS,
    SFX_MAGIC_SHAKE, SFX_MAGIC_YES, SFX_MAGIC_NO, SFX_MAGIC_MAYBE, SFX_MAGIC_HOT,
    SFX_SNAKE_EAT, SFX_SNAKE_DIE,
    SFX_SPOON_BOING, SFX_SPOON_FALL, SFX_SPOON_PERFECT, SFX_SPOON_JELLY, SFX_SPOON_CRACK,
    SFX_SPOON_SOGGY, SFX_SPOON_COIN, SFX_SPOON_STOMP, SFX_SPOON_POWER, SFX_SPOON_SHIELD,
    SFX_SPOON_WARN, SFX_SPOON_PHASE, SFX_SPOON_BEST, SFX_SPOON_HIT, SFX_SPOON_SPLASH,
    SFX_SPOON_SECRET, SFX_SPOON_GOLDEN, SFX_SPOON_LOST,
    SFX_RAGDOLL_GRAB, SFX_RAGDOLL_THUD,
    SFX_FM_TAP, SFX_FM_BUY,
    SFX_CHESS_SELECT, SFX_CHESS_MOVE, SFX_CHESS_CAPTURE, SFX_CHESS_CHECK,
    SFX_CHESS_UNDO, SFX_CHESS_PROMOTE,
    SFX_QUIZ_QUESTION, SFX_QUIZ_RIGHT, SFX_QUIZ_STREAK, SFX_QUIZ_WRONG,
    SFX_QUIZ_TIMEUP, SFX_QUIZ_TICK, SFX_QUIZ_POP, SFX_QUIZ_CASH, SFX_QUIZ_ZAP,
    SFX_QUIZ_APPLAUSE,
    SFX_PET_CHIRP, SFX_PET_CHOMP, SFX_PET_BUBBLE, SFX_PET_LULLABY, SFX_PET_EKG,
    SFX_PET_BEAT, SFX_PET_CALM, SFX_PET_WILD, SFX_PET_POKE, SFX_PET_FAREWELL,
    SFX_PET_GHOST, SFX_PET_HATCH, SFX_PET_DONE, SFX_PET_SELECT,
    SFX_PET_CATCH, SFX_PET_MISS, SFX_PET_SWAP, SFX_PET_GEM, SFX_PET_YES,
    SFX_PET_NOPE, SFX_PET_WIN, SFX_PET_LOSE, SFX_PET_RIP,
    SFX_VINYL_DROP, SFX_VINYL_SCRATCH, SFX_TAPE_CLICK,
    /* the batch 9 games: Snake's golden apple, 2048, Sudoku, 24 Points; Flappy Bird */
    SFX_SNAKE_GOLD, SFX_TILE_SLIDE, SFX_TILE_MERGE, SFX_SUDOKU_PLACE, SFX_POINTS_JOIN,
    SFX_FB_WING, SFX_FB_POINT, SFX_FB_HIT, SFX_FB_DIE, SFX_FB_SWOOSH,
    /* the interface (Settings > Sounds > Interface sounds) */
    SFX_UI_OPEN, SFX_UI_BACK, SFX_UI_ON, SFX_UI_OFF, SFX_UI_LOCK, SFX_UI_UNLOCK,
    SFX_UI_PANEL_OPEN, SFX_UI_PANEL_CLOSE, SFX_UI_NOTIFY, SFX_UI_PICKUP, SFX_UI_DROP,
    SFX_UI_REMOVE, SFX_UI_TICK, SFX_UI_SHUTTER,
    /* power and battery; also Interface sounds, whatever their priority */
    SFX_SYS_BOOT, SFX_SYS_SHUTDOWN, SFX_SYS_CHARGE, SFX_SYS_UNPLUG, SFX_SYS_BATT_LOW,
    SFX_COUNT
};

static const struct sfx_def { uint8_t prio; struct sfx_note n[SFX_NOTES]; } sfx_defs[SFX_COUNT] = {
    /* ---- shared */
    [SFX_TAP]   = { P_TICK,   { N(0, 28, 1800, 1500, W_SINE, E_PLUCK, 35) } },
    /* a soft "uh-uh": triangles, since a square this low is the harshest
     * thing the system could make and errors can repeat */
    [SFX_ERROR] = { P_NORMAL, { N(0, 90, 330, 311, W_TRI, E_HOLD, 36),
                                N(115, 150, 262, 247, W_TRI, E_HOLD, 36) } },
    [SFX_WIN]   = { P_RESULT, { N(0, 120, 523, 523, W_TRI, E_PLUCK, 60),
                                N(100, 120, 659, 659, W_TRI, E_PLUCK, 60),
                                N(200, 120, 784, 784, W_TRI, E_PLUCK, 60),
                                N(300, 480, 1047, 1047, W_TRI, E_PLUCK, 66),
                                N(300, 480, 2093, 2093, W_SINE, E_PLUCK, 16) } },
    [SFX_LOSE]  = { P_RESULT, { N(0, 170, 392, 392, W_TRI, E_HOLD, 50),
                                N(190, 170, 330, 330, W_TRI, E_HOLD, 50),
                                N(380, 520, 262, 240, W_TRI, E_PLUCK, 56) } },
    [SFX_DRAW]  = { P_RESULT, { N(0, 160, 587, 587, W_TRI, E_PLUCK, 50),
                                N(180, 420, 587, 587, W_TRI, E_PLUCK, 50),
                                N(180, 420, 880, 880, W_SINE, E_PLUCK, 22) } },
    [SFX_START] = { P_NORMAL, { N(0, 70, 660, 880, W_SQUARE, E_PLUCK, 48),
                                N(70, 130, 880, 1320, W_SQUARE, E_PLUCK, 48) } },
    /* ---- Flip a Coin: a thumb flick that rings, the whir of the spin, then
     * a clink and a small bounce as it lands. */
    [SFX_COIN_FLIP]  = { P_NORMAL, { N(0, 320, 2600, 2600, W_BELL, E_PLUCK, 50),
                                     N(30, 1000, 2600, 900, W_NOISE, E_SWELL, 16),
                                     N(60, 900, 1300, 1150, W_SINE, E_SWELL, 8) } },
    [SFX_COIN_LAND]  = { P_NORMAL, { N(0, 40, 700, 700, W_NOISE, E_PLUCK, 38),
                                     N(0, 200, 3100, 3100, W_BELL, E_PLUCK, 55),
                                     N(115, 140, 3300, 3300, W_BELL, E_PLUCK, 28),
                                     N(190, 90, 3400, 3400, W_BELL, E_PLUCK, 12) } },
    [SFX_COIN_HEADS] = { P_RESULT, { N(0, 100, 784, 784, W_TRI, E_PLUCK, 42),
                                     N(95, 260, 1175, 1175, W_TRI, E_PLUCK, 48) } },
    [SFX_COIN_TAILS] = { P_RESULT, { N(0, 100, 784, 784, W_TRI, E_PLUCK, 42),
                                     N(95, 260, 587, 587, W_TRI, E_PLUCK, 48) } },
    /* ---- Magic 8 Ball: liquid sloshing over a low hum, then a shimmer
     * that leans yes, no or maybe; and a wolf whistle for the rare one. */
    [SFX_MAGIC_SHAKE] = { P_NORMAL, { N(0, 240, 650, 650, W_NOISE, E_SWELL, 42),
                                      N(240, 240, 560, 560, W_NOISE, E_SWELL, 38),
                                      N(480, 240, 480, 480, W_NOISE, E_SWELL, 30),
                                      N(720, 180, 420, 420, W_NOISE, E_SWELL, 20),
                                      N(0, 900, 110, 131, W_SINE, E_SWELL, 26),
                                      N(0, 900, 220, 262, W_SINE, E_SWELL, 8) } },
    [SFX_MAGIC_YES]   = { P_RESULT, { N(0, 600, 1047, 1047, W_SINE, E_PLUCK, 34),
                                      N(70, 600, 1319, 1319, W_SINE, E_PLUCK, 32),
                                      N(140, 600, 1568, 1568, W_SINE, E_PLUCK, 30),
                                      N(210, 700, 2093, 2093, W_SINE, E_PLUCK, 26),
                                      N(0, 800, 523, 523, W_TRI, E_SWELL, 20) } },
    [SFX_MAGIC_NO]    = { P_RESULT, { N(0, 600, 1760, 1760, W_SINE, E_PLUCK, 30),
                                      N(80, 600, 1397, 1397, W_SINE, E_PLUCK, 30),
                                      N(160, 700, 1175, 1175, W_SINE, E_PLUCK, 30),
                                      N(0, 900, 147, 139, W_TRI, E_SWELL, 26) } },
    [SFX_MAGIC_MAYBE] = { P_RESULT, { N(0, 450, 1047, 1175, W_SINE, E_PLUCK, 30),
                                      N(90, 450, 1319, 1480, W_SINE, E_PLUCK, 28),
                                      N(180, 450, 1175, 1047, W_SINE, E_PLUCK, 28),
                                      N(270, 600, 1480, 1661, W_SINE, E_PLUCK, 24),
                                      N(0, 900, 208, 208, W_TRI, E_SWELL, 18) } },
    [SFX_MAGIC_HOT]   = { P_RESULT, { N(0, 230, 900, 2600, W_SINE, E_HOLD, 42),
                                      N(340, 200, 1200, 2900, W_SINE, E_HOLD, 42),
                                      N(540, 380, 2900, 900, W_SINE, E_PLUCK, 42) } },
    /* ---- Snake */
    /* a round little gulp, and a soft bonk that slides down */
    [SFX_SNAKE_EAT] = { P_NORMAL, { N(0, 70, 520, 880, W_SINE, E_PLUCK, 58),
                                    N(45, 120, 880, 1320, W_TRI, E_PLUCK, 40) } },
    [SFX_SNAKE_DIE] = { P_RESULT, { N(0, 150, 220, 110, W_SINE, E_PLUCK, 55),
                                    N(90, 460, 392, 147, W_TRI, E_PLUCK, 42) } },
    /* ---- Freaky Spoon: a springy boing, and a slide whistle down to a thud. */
    [SFX_SPOON_BOING] = { P_TICK,   { N(0, 190, 180, 520, W_SINE, E_PLUCK, 48),
                                      N(0, 190, 360, 1040, W_TRI, E_PLUCK, 12) } },
    [SFX_SPOON_FALL]  = { P_RESULT, { N(0, 700, 1400, 220, W_SINE, E_HOLD, 36),
                                      N(700, 120, 300, 300, W_NOISE, E_PLUCK, 50),
                                      N(700, 170, 95, 50, W_SINE, E_PLUCK, 60) } },
    /* a bright ding; the game raises its pitch with the combo */
    [SFX_SPOON_PERFECT] = { P_TICK,   { N(0, 160, 1568, 1568, W_BELL, E_PLUCK, 36),
                                        N(0, 90, 784, 1568, W_SINE, E_PLUCK, 26) } },
    /* a wobbly jelly boing, long and silly */
    [SFX_SPOON_JELLY]   = { P_NORMAL, { N(0, 420, 140, 620, W_SINE, E_PLUCK, 56),
                                        N(0, 420, 147, 640, W_SINE, E_PLUCK, 30),
                                        N(60, 300, 900, 1800, W_TRI, E_PLUCK, 12) } },
    [SFX_SPOON_CRACK]   = { P_TICK,   { N(0, 60, 2600, 2600, W_NOISE, E_PLUCK, 40),
                                        N(40, 90, 900, 900, W_NOISE, E_PLUCK, 22) } },
    /* a wet squelch as the toast lets go */
    [SFX_SPOON_SOGGY]   = { P_NORMAL, { N(0, 200, 400, 120, W_TRI, E_PLUCK, 40),
                                        N(0, 160, 600, 600, W_NOISE, E_PLUCK, 18) } },
    [SFX_SPOON_COIN]    = { P_TICK,   { N(0, 50, 1976, 1976, W_SQUARE, E_PLUCK, 24),
                                        N(45, 140, 2637, 2637, W_SQUARE, E_PLUCK, 24) } },
    [SFX_SPOON_STOMP]   = { P_NORMAL, { N(0, 80, 260, 90, W_SINE, E_PLUCK, 62),
                                        N(0, 50, 1800, 1800, W_NOISE, E_PLUCK, 30),
                                        N(70, 160, 660, 1320, W_SQUARE, E_PLUCK, 26) } },
    [SFX_SPOON_POWER]   = { P_NORMAL, { N(0, 80, 523, 523, W_SQUARE, E_PLUCK, 34),
                                        N(70, 80, 784, 784, W_SQUARE, E_PLUCK, 34),
                                        N(140, 80, 1047, 1047, W_SQUARE, E_PLUCK, 34),
                                        N(210, 260, 1568, 1568, W_TRI, E_PLUCK, 38) } },
    [SFX_SPOON_SHIELD]  = { P_NORMAL, { N(0, 300, 3000, 3000, W_BELL, E_PLUCK, 40),
                                        N(0, 120, 400, 400, W_NOISE, E_PLUCK, 30) } },
    /* two quick sirens: something is coming */
    [SFX_SPOON_WARN]    = { P_NORMAL, { N(0, 140, 880, 1175, W_SQUARE, E_HOLD, 26),
                                        N(170, 140, 880, 1175, W_SQUARE, E_HOLD, 26) } },
    [SFX_SPOON_PHASE]   = { P_RESULT, { N(0, 100, 659, 659, W_SQUARE, E_PLUCK, 34),
                                        N(100, 100, 784, 784, W_SQUARE, E_PLUCK, 34),
                                        N(200, 100, 988, 988, W_SQUARE, E_PLUCK, 34),
                                        N(300, 420, 1319, 1319, W_TRI, E_PLUCK, 44),
                                        N(300, 420, 659, 659, W_TRI, E_PLUCK, 22) } },
    [SFX_SPOON_BEST]    = { P_RESULT, { N(0, 90, 1047, 1047, W_TRI, E_PLUCK, 42),
                                        N(90, 90, 1319, 1319, W_TRI, E_PLUCK, 42),
                                        N(180, 90, 1568, 1568, W_TRI, E_PLUCK, 42),
                                        N(270, 500, 2093, 2093, W_TRI, E_PLUCK, 46),
                                        N(270, 500, 3136, 3136, W_BELL, E_PLUCK, 14) } },
    [SFX_SPOON_HIT]     = { P_RESULT, { N(0, 120, 1200, 300, W_SQUARE, E_PLUCK, 40),
                                        N(0, 80, 900, 900, W_NOISE, E_PLUCK, 44),
                                        N(120, 500, 500, 90, W_TRI, E_HOLD, 40) } },
    [SFX_SPOON_SPLASH]  = { P_RESULT, { N(0, 380, 1400, 300, W_NOISE, E_PLUCK, 46),
                                        N(0, 240, 180, 70, W_SINE, E_PLUCK, 56),
                                        N(200, 300, 300, 800, W_SINE, E_PLUCK, 14) } },
    /* a spooky little arpeggio for a secret */
    [SFX_SPOON_SECRET]  = { P_RESULT, { N(0, 120, 988, 988, W_SINE, E_PLUCK, 34),
                                        N(110, 120, 1245, 1245, W_SINE, E_PLUCK, 34),
                                        N(220, 120, 1480, 1480, W_SINE, E_PLUCK, 34),
                                        N(330, 600, 1976, 1976, W_BELL, E_PLUCK, 34) } },
    [SFX_SPOON_GOLDEN]  = { P_RESULT, { N(0, 700, 2093, 2093, W_BELL, E_PLUCK, 40),
                                        N(60, 700, 2637, 2637, W_BELL, E_PLUCK, 34),
                                        N(120, 700, 3136, 3136, W_BELL, E_PLUCK, 30),
                                        N(180, 800, 4186, 4186, W_BELL, E_PLUCK, 24) } },
    /* a sad descending wah when a big combo breaks */
    [SFX_SPOON_LOST]    = { P_NORMAL, { N(0, 260, 523, 392, W_TRI, E_HOLD, 30) } },
    /* ---- Ragdoll */
    [SFX_RAGDOLL_GRAB] = { P_TICK,   { N(0, 70, 480, 700, W_SINE, E_PLUCK, 30) } },
    [SFX_RAGDOLL_THUD] = { P_NORMAL, { N(0, 170, 130, 45, W_SINE, E_PLUCK, 64),
                                       N(0, 60, 420, 420, W_NOISE, E_PLUCK, 40) } },
    /* ---- Sloppy FM: a coin blip through a crackle of static; a till. */
    [SFX_FM_TAP] = { P_TICK,   { N(0, 40, 5000, 5000, W_NOISE, E_PLUCK, 10),
                                 N(0, 50, 988, 988, W_SQUARE, E_PLUCK, 44),
                                 N(45, 110, 1319, 1319, W_SQUARE, E_PLUCK, 44) } },
    [SFX_FM_BUY] = { P_RESULT, { N(0, 70, 3000, 3000, W_NOISE, E_PLUCK, 30),
                                 N(60, 450, 2093, 2093, W_BELL, E_PLUCK, 42),
                                 N(95, 450, 2637, 2637, W_BELL, E_PLUCK, 34) } },
    /* ---- Chess: wooden pieces on a board. */
    [SFX_CHESS_SELECT]  = { P_TICK,   { N(0, 30, 1200, 1200, W_SINE, E_PLUCK, 22) } },
    [SFX_CHESS_MOVE]    = { P_NORMAL, { N(0, 75, 520, 380, W_SINE, E_PLUCK, 55),
                                        N(0, 18, 1800, 1800, W_NOISE, E_PLUCK, 30) } },
    [SFX_CHESS_CAPTURE] = { P_NORMAL, { N(0, 110, 400, 260, W_SINE, E_PLUCK, 60),
                                        N(0, 35, 1200, 1200, W_NOISE, E_PLUCK, 45),
                                        N(45, 70, 620, 460, W_SINE, E_PLUCK, 36),
                                        N(45, 20, 1800, 1800, W_NOISE, E_PLUCK, 22) } },
    [SFX_CHESS_CHECK]   = { P_NORMAL, { N(0, 75, 520, 380, W_SINE, E_PLUCK, 55),
                                        N(0, 18, 1800, 1800, W_NOISE, E_PLUCK, 30),
                                        N(110, 90, 880, 880, W_TRI, E_HOLD, 38),
                                        N(220, 200, 1175, 1175, W_TRI, E_PLUCK, 44) } },
    [SFX_CHESS_UNDO]    = { P_NORMAL, { N(0, 150, 900, 480, W_SINE, E_PLUCK, 32),
                                        N(0, 150, 2000, 2000, W_NOISE, E_SWELL, 14) } },
    [SFX_CHESS_PROMOTE] = { P_NORMAL, { N(0, 75, 520, 380, W_SINE, E_PLUCK, 50),
                                        N(60, 260, 1047, 1047, W_SINE, E_PLUCK, 30),
                                        N(110, 260, 1319, 1319, W_SINE, E_PLUCK, 30),
                                        N(160, 260, 1568, 1568, W_SINE, E_PLUCK, 30),
                                        N(210, 420, 2093, 2093, W_SINE, E_PLUCK, 28) } },
    /* ---- Music Bip Bop Smarty: game show cues. */
    [SFX_QUIZ_QUESTION] = { P_NORMAL, { N(0, 130, 784, 784, W_TRI, E_PLUCK, 42),
                                        N(115, 280, 1047, 1047, W_TRI, E_PLUCK, 46) } },
    [SFX_QUIZ_RIGHT]    = { P_RESULT, { N(0, 100, 1047, 1047, W_TRI, E_PLUCK, 50),
                                        N(85, 330, 1568, 1568, W_TRI, E_PLUCK, 54),
                                        N(85, 330, 3136, 3136, W_SINE, E_PLUCK, 14) } },
    [SFX_QUIZ_STREAK]   = { P_RESULT, { N(0, 100, 1047, 1047, W_TRI, E_PLUCK, 50),
                                        N(85, 200, 1568, 1568, W_TRI, E_PLUCK, 50),
                                        N(250, 110, 1319, 1319, W_SINE, E_PLUCK, 30),
                                        N(310, 110, 1568, 1568, W_SINE, E_PLUCK, 30),
                                        N(370, 110, 2093, 2093, W_SINE, E_PLUCK, 30),
                                        N(430, 420, 2637, 2637, W_SINE, E_PLUCK, 30) } },
    [SFX_QUIZ_WRONG]    = { P_RESULT, { N(0, 380, 150, 146, W_SQUARE, E_HOLD, 26),
                                        N(0, 380, 157, 152, W_SQUARE, E_HOLD, 22) } },
    [SFX_QUIZ_TIMEUP]   = { P_RESULT, { N(0, 210, 392, 370, W_TRI, E_HOLD, 46),
                                        N(230, 210, 370, 349, W_TRI, E_HOLD, 46),
                                        N(460, 520, 349, 300, W_TRI, E_PLUCK, 50) } },
    [SFX_QUIZ_TICK]     = { P_TICK,   { N(0, 24, 2000, 2000, W_SINE, E_PLUCK, 30),
                                        N(0, 10, 6000, 6000, W_NOISE, E_PLUCK, 10) } },
    [SFX_QUIZ_POP]      = { P_TICK,   { N(0, 55, 700, 1100, W_SINE, E_PLUCK, 26) } },
    /* the till: drawer clack, then two bells */
    [SFX_QUIZ_CASH]     = { P_NORMAL, { N(0, 40, 2400, 2400, W_NOISE, E_PLUCK, 30),
                                        N(0, 45, 220, 150, W_SQUARE, E_PLUCK, 20),
                                        N(60, 380, 2637, 2637, W_BELL, E_PLUCK, 34),
                                        N(150, 520, 3520, 3520, W_BELL, E_PLUCK, 30) } },
    /* Lightning Round: a crackle and a rising zap */
    [SFX_QUIZ_ZAP]      = { P_RESULT, { N(0, 220, 4000, 4000, W_NOISE, E_PLUCK, 34),
                                        N(0, 260, 200, 1600, W_SQUARE, E_PLUCK, 26),
                                        N(240, 200, 800, 2400, W_SQUARE, E_PLUCK, 22),
                                        N(420, 420, 1568, 1568, W_TRI, E_PLUCK, 40) } },
    /* a studio audience: swelling noise with claps in it, and a fanfare */
    [SFX_QUIZ_APPLAUSE] = { P_RESULT, { N(0, 1500, 3000, 2600, W_NOISE, E_SWELL, 26),
                                        N(80, 40, 1800, 1800, W_NOISE, E_PLUCK, 30),
                                        N(290, 40, 2100, 2100, W_NOISE, E_PLUCK, 30),
                                        N(520, 40, 1700, 1700, W_NOISE, E_PLUCK, 30),
                                        N(0, 140, 784, 784, W_TRI, E_PLUCK, 46),
                                        N(130, 140, 1047, 1047, W_TRI, E_PLUCK, 46),
                                        N(260, 600, 1319, 1319, W_TRI, E_PLUCK, 50),
                                        N(260, 600, 2637, 2637, W_SINE, E_PLUCK, 14) } },
    /* ---- Sloppy Gotchi: 8-bit chirps and little scene sounds. */
    [SFX_PET_CHIRP]  = { P_TICK,   { N(0, 50, 1568, 1760, W_SQUARE, E_PLUCK, 40),
                                     N(60, 90, 2093, 2349, W_SQUARE, E_PLUCK, 40) } },
    [SFX_PET_CHOMP]  = { P_TICK,   { N(0, 70, 900, 900, W_NOISE, E_PLUCK, 40),
                                     N(0, 60, 180, 120, W_SQUARE, E_PLUCK, 35) } },
    [SFX_PET_BUBBLE] = { P_TICK,   { N(0, 60, 600, 1400, W_SINE, E_PLUCK, 30) } },
    [SFX_PET_LULLABY]= { P_NORMAL, { N(0, 500, 1568, 1568, W_SINE, E_PLUCK, 26),
                                     N(300, 500, 1319, 1319, W_SINE, E_PLUCK, 26),
                                     N(600, 500, 1047, 1047, W_SINE, E_PLUCK, 26),
                                     N(900, 500, 1319, 1319, W_SINE, E_PLUCK, 24),
                                     N(1200, 900, 1175, 1175, W_SINE, E_PLUCK, 24),
                                     N(0, 2100, 262, 262, W_TRI, E_SWELL, 12) } },
    [SFX_PET_EKG]    = { P_TICK,   { N(0, 80, 1000, 1000, W_SINE, E_HOLD, 24) } },
    [SFX_PET_BEAT]   = { P_TICK,   { N(0, 120, 150, 45, W_SINE, E_PLUCK, 66),
                                     N(235, 30, 7000, 7000, W_NOISE, E_PLUCK, 16) } },
    [SFX_PET_CALM]   = { P_NORMAL, { N(0, 900, 523, 523, W_SINE, E_PLUCK, 30),
                                     N(150, 900, 784, 784, W_SINE, E_PLUCK, 26),
                                     N(300, 1000, 1047, 1047, W_SINE, E_PLUCK, 22) } },
    [SFX_PET_WILD]   = { P_NORMAL, { N(0, 150, 300, 1500, W_SQUARE, E_HOLD, 44),
                                     N(150, 150, 1500, 300, W_SQUARE, E_HOLD, 44),
                                     N(300, 200, 300, 2000, W_SQUARE, E_PLUCK, 44) } },
    [SFX_PET_POKE]   = { P_TICK,   { N(0, 60, 300, 200, W_SINE, E_PLUCK, 42),
                                     N(0, 22, 1500, 1500, W_NOISE, E_PLUCK, 26) } },
    [SFX_PET_FAREWELL] = { P_RESULT, { N(0, 700, 1568, 1568, W_SINE, E_PLUCK, 24),
                                       N(450, 700, 1319, 1319, W_SINE, E_PLUCK, 24),
                                       N(900, 700, 1175, 1175, W_SINE, E_PLUCK, 24),
                                       N(1350, 1300, 1047, 1047, W_SINE, E_PLUCK, 24),
                                       N(0, 2600, 262, 262, W_TRI, E_SWELL, 12) } },
    [SFX_PET_GHOST]  = { P_NORMAL, { N(0, 1300, 700, 1400, W_SINE, E_SWELL, 16),
                                     N(0, 1300, 706, 1412, W_SINE, E_SWELL, 12) } },
    [SFX_PET_HATCH]  = { P_RESULT, { N(0, 40, 2500, 2500, W_NOISE, E_PLUCK, 36),
                                     N(130, 40, 2800, 2800, W_NOISE, E_PLUCK, 36),
                                     N(260, 80, 1047, 1047, W_SQUARE, E_PLUCK, 44),
                                     N(330, 80, 1319, 1319, W_SQUARE, E_PLUCK, 44),
                                     N(400, 80, 1568, 1568, W_SQUARE, E_PLUCK, 44),
                                     N(470, 260, 2093, 2093, W_SQUARE, E_PLUCK, 44) } },
    [SFX_PET_DONE]   = { P_NORMAL, { N(0, 70, 1319, 1319, W_SQUARE, E_PLUCK, 40),
                                     N(80, 70, 1568, 1568, W_SQUARE, E_PLUCK, 40),
                                     N(160, 200, 2093, 2093, W_SQUARE, E_PLUCK, 40) } },
    [SFX_PET_SELECT] = { P_TICK,   { N(0, 40, 1319, 1319, W_SQUARE, E_PLUCK, 35) } },
    [SFX_PET_CATCH]  = { P_NORMAL, { N(0, 90, 880, 1760, W_SQUARE, E_PLUCK, 48),
                                     N(60, 90, 1760, 2637, W_SQUARE, E_PLUCK, 44) } },
    [SFX_PET_MISS]   = { P_TICK,   { N(0, 90, 220, 180, W_SQUARE, E_PLUCK, 44) } },
    [SFX_PET_SWAP]   = { P_TICK,   { N(0, 220, 1500, 1500, W_NOISE, E_SWELL, 18),
                                     N(0, 220, 400, 520, W_SINE, E_SWELL, 12) } },
    [SFX_PET_GEM]    = { P_NORMAL, { N(0, 300, 523, 523, W_TRI, E_PLUCK, 46),
                                     N(0, 200, 1046, 1046, W_SQUARE, E_PLUCK, 18) } },
    [SFX_PET_YES]    = { P_NORMAL, { N(0, 60, 1047, 1047, W_SQUARE, E_PLUCK, 44),
                                     N(70, 150, 1568, 1568, W_SQUARE, E_PLUCK, 44) } },
    [SFX_PET_NOPE]   = { P_NORMAL, { N(0, 90, 330, 330, W_SQUARE, E_HOLD, 40),
                                     N(105, 190, 262, 247, W_SQUARE, E_HOLD, 40) } },
    [SFX_PET_WIN]    = { P_RESULT, { N(0, 80, 1047, 1047, W_SQUARE, E_PLUCK, 40),
                                     N(90, 80, 1319, 1319, W_SQUARE, E_PLUCK, 40),
                                     N(180, 80, 1568, 1568, W_SQUARE, E_PLUCK, 40),
                                     N(270, 80, 2093, 2093, W_SQUARE, E_PLUCK, 40),
                                     N(360, 60, 1568, 1568, W_SQUARE, E_PLUCK, 40),
                                     N(430, 320, 2093, 2093, W_SQUARE, E_PLUCK, 44) } },
    [SFX_PET_LOSE]   = { P_RESULT, { N(0, 160, 784, 784, W_SQUARE, E_HOLD, 35),
                                     N(180, 160, 659, 659, W_SQUARE, E_HOLD, 35),
                                     N(360, 360, 523, 494, W_SQUARE, E_PLUCK, 40) } },
    [SFX_PET_RIP]    = { P_RESULT, { N(0, 250, 523, 523, W_SQUARE, E_HOLD, 35),
                                     N(280, 250, 494, 494, W_SQUARE, E_HOLD, 35),
                                     N(560, 250, 466, 466, W_SQUARE, E_HOLD, 35),
                                     N(840, 700, 440, 415, W_SQUARE, E_PLUCK, 40) } },
    /* ---- Vinyl: the needle lands with a thump and a few pops of dust; a
     * scratch is a burst of noise swept up and back; a cassette clicks in. */
    [SFX_VINYL_DROP]    = { P_NORMAL, { N(0, 90, 70, 45, W_SINE, E_PLUCK, 55),
                                         N(0, 30, 900, 900, W_NOISE, E_PLUCK, 34),
                                         N(140, 12, 3500, 3500, W_NOISE, E_PLUCK, 30),
                                         N(260, 10, 5000, 5000, W_NOISE, E_PLUCK, 24),
                                         N(330, 14, 2800, 2800, W_NOISE, E_PLUCK, 26),
                                         N(470, 10, 4200, 4200, W_NOISE, E_PLUCK, 18),
                                         N(0, 700, 6000, 6000, W_NOISE, E_SWELL, 6) } },
    [SFX_VINYL_SCRATCH] = { P_TICK,   { N(0, 70, 900, 2600, W_NOISE, E_HOLD, 30),
                                         N(70, 80, 2600, 700, W_NOISE, E_PLUCK, 26) } },
    [SFX_TAPE_CLICK]    = { P_NORMAL, { N(0, 25, 1400, 1400, W_NOISE, E_PLUCK, 40),
                                         N(0, 40, 180, 140, W_SINE, E_PLUCK, 40),
                                         N(120, 25, 1800, 1800, W_NOISE, E_PLUCK, 34),
                                         N(180, 500, 300, 300, W_NOISE, E_SWELL, 8) } },
    /* ---- the interface: short, soft, and quieter than any app sound */
    [SFX_UI_OPEN]  = { P_UI, { N(0, 22, 1500, 1850, W_SINE, E_PLUCK, 20),
                               N(0, 8, 5000, 5000, W_NOISE, E_PLUCK, 6) } },
    [SFX_UI_BACK]  = { P_UI, { N(0, 24, 1250, 950, W_SINE, E_PLUCK, 18),
                               N(0, 8, 4000, 4000, W_NOISE, E_PLUCK, 5) } },
    [SFX_UI_ON]    = { P_UI, { N(0, 14, 900, 900, W_SINE, E_PLUCK, 24),
                               N(45, 26, 1600, 1600, W_SINE, E_PLUCK, 24) } },
    [SFX_UI_OFF]   = { P_UI, { N(0, 14, 1600, 1600, W_SINE, E_PLUCK, 22),
                               N(45, 26, 900, 900, W_SINE, E_PLUCK, 22) } },
    [SFX_UI_LOCK]  = { P_UI, { N(0, 24, 2200, 2200, W_NOISE, E_PLUCK, 24),
                               N(0, 80, 320, 170, W_SINE, E_PLUCK, 30),
                               N(40, 30, 1800, 1800, W_NOISE, E_PLUCK, 18) } },
    [SFX_UI_UNLOCK]= { P_UI, { N(0, 18, 2600, 2600, W_NOISE, E_PLUCK, 20),
                               N(10, 160, 784, 784, W_BELL, E_PLUCK, 24),
                               N(80, 260, 1175, 1175, W_BELL, E_PLUCK, 24) } },
    [SFX_UI_PANEL_OPEN]  = { P_UI, { N(0, 200, 2400, 2400, W_NOISE, E_SWELL, 12),
                                     N(0, 200, 520, 780, W_SINE, E_SWELL, 10) } },
    [SFX_UI_PANEL_CLOSE] = { P_UI, { N(0, 180, 1800, 1800, W_NOISE, E_SWELL, 11),
                                     N(0, 180, 780, 520, W_SINE, E_SWELL, 9) } },
    [SFX_UI_NOTIFY] = { P_UI, { N(0, 90, 1319, 1319, W_TRI, E_PLUCK, 30),
                                N(80, 220, 1760, 1760, W_TRI, E_PLUCK, 30) } },
    [SFX_UI_PICKUP] = { P_UI, { N(0, 70, 480, 900, W_SINE, E_PLUCK, 34) } },
    [SFX_UI_DROP]   = { P_UI, { N(0, 70, 330, 170, W_SINE, E_PLUCK, 30),
                                N(0, 14, 1600, 1600, W_NOISE, E_PLUCK, 16) } },
    [SFX_UI_REMOVE] = { P_UI, { N(0, 170, 2500, 2500, W_NOISE, E_SWELL, 16),
                                N(0, 170, 900, 380, W_SINE, E_PLUCK, 24) } },
    /* a comb tooth: the A-Z rail and volume steps pitch it by position */
    [SFX_UI_TICK]   = { P_UI, { N(0, 18, 1500, 1420, W_SINE, E_PLUCK, 16),
                                N(0, 6, 4000, 4000, W_NOISE, E_PLUCK, 4) } },
    /* a shutter: the blades close and open again over a soft thump */
    [SFX_UI_SHUTTER] = { P_UI, { N(0, 16, 3400, 3400, W_NOISE, E_PLUCK, 20),
                                 N(0, 40, 240, 150, W_SINE, E_PLUCK, 16),
                                 N(60, 22, 2600, 2600, W_NOISE, E_PLUCK, 16) } },
    /* ---- power. The system's motif is a rising fifth, G5 to D6, the
     * unlock's two bells: power on plays it in full, shutdown backwards. */
    [SFX_SYS_BOOT]     = { P_RESULT, { N(0, 400, 1800, 2600, W_NOISE, E_SWELL, 10),
                                       N(250, 400, 784, 784, W_BELL, E_PLUCK, 30),
                                       N(380, 500, 1175, 1175, W_BELL, E_PLUCK, 30),
                                       N(380, 500, 2350, 2350, W_SINE, E_PLUCK, 8),
                                       N(250, 630, 392, 392, W_TRI, E_SWELL, 14) } },
    [SFX_SYS_SHUTDOWN] = { P_RESULT, { N(0, 300, 1175, 1175, W_BELL, E_PLUCK, 22),
                                       N(160, 420, 784, 784, W_BELL, E_PLUCK, 22),
                                       N(0, 580, 392, 370, W_TRI, E_SWELL, 12) } },
    /* energy in: a glide up onto one bell; unplugging is the glide down */
    [SFX_SYS_CHARGE]   = { P_UI, { N(0, 90, 523, 1047, W_SINE, E_PLUCK, 22),
                                   N(80, 220, 1568, 1568, W_BELL, E_PLUCK, 18) } },
    [SFX_SYS_UNPLUG]   = { P_UI, { N(0, 110, 1047, 523, W_SINE, E_PLUCK, 20) } },
    /* concerned, not alarmed: G5 falling to E5, once per threshold */
    [SFX_SYS_BATT_LOW] = { P_NORMAL, { N(0, 120, 784, 784, W_TRI, E_PLUCK, 28),
                                       N(160, 220, 659, 640, W_TRI, E_PLUCK, 28) } },
    /* ---- the batch 9 games */
    /* Snake's golden apple: three bells going up */
    [SFX_SNAKE_GOLD]   = { P_NORMAL, { N(0, 90, 1319, 1319, W_BELL, E_PLUCK, 40),
                                       N(70, 100, 1760, 1760, W_BELL, E_PLUCK, 36),
                                       N(140, 220, 2637, 2637, W_BELL, E_PLUCK, 30) } },
    /* 2048: a soft swish for a slide, a round pop for a join (pitched up by the tile) */
    [SFX_TILE_SLIDE]   = { P_TICK,   { N(0, 70, 1600, 700, W_NOISE, E_PLUCK, 16) } },
    [SFX_TILE_MERGE]   = { P_NORMAL, { N(0, 80, 440, 660, W_SINE, E_PLUCK, 50),
                                       N(30, 110, 880, 990, W_TRI, E_PLUCK, 24) } },
    /* Sudoku: a pencil's tick */
    [SFX_SUDOKU_PLACE] = { P_TICK,   { N(0, 45, 1250, 1100, W_SINE, E_PLUCK, 34),
                                       N(0, 20, 3000, 2000, W_NOISE, E_PLUCK, 8) } },
    /* 24 Points: two cards become one */
    [SFX_POINTS_JOIN]  = { P_NORMAL, { N(0, 70, 660, 880, W_TRI, E_PLUCK, 40),
                                       N(45, 100, 1320, 1320, W_SINE, E_PLUCK, 24) } },
    /* Flappy Bird: a wingbeat (a puff of air and a soft thump), a bright
     * two-note ding through a pipe, a smack on a hit, the whistle of the
     * fall, and the swoosh that brings the score in */
    [SFX_FB_WING]      = { P_TICK,   { N(0, 60, 1500, 1500, W_NOISE, E_PLUCK, 34),
                                       N(0, 70, 440, 260, W_SINE, E_PLUCK, 22) } },
    [SFX_FB_POINT]     = { P_NORMAL, { N(0, 60, 1319, 1319, W_SQUARE, E_PLUCK, 18),
                                       N(55, 150, 1760, 1760, W_SQUARE, E_PLUCK, 18),
                                       N(55, 150, 3520, 3520, W_SINE, E_PLUCK, 8) } },
    [SFX_FB_HIT]       = { P_RESULT, { N(0, 45, 3200, 3200, W_NOISE, E_PLUCK, 56),
                                       N(0, 120, 190, 70, W_SINE, E_PLUCK, 60),
                                       N(5, 60, 900, 420, W_SQUARE, E_PLUCK, 14) } },
    [SFX_FB_DIE]       = { P_RESULT, { N(0, 540, 1050, 190, W_TRI, E_HOLD, 36),
                                       N(0, 540, 2100, 380, W_SINE, E_HOLD, 9) } },
    [SFX_FB_SWOOSH]    = { P_NORMAL, { N(0, 300, 1900, 1900, W_NOISE, E_SWELL, 30),
                                       N(0, 300, 320, 900, W_SINE, E_SWELL, 6) } },
};

/* ------------------------------------------------------------- engine */
#define SFX_CHUNK 512                  /* frames per callback */
struct sfx_voice {
    uint32_t start, len, k;            /* in samples */
    uint32_t phase, step, phase2;
    int32_t dstep;                     /* glide, per sample */
    int32_t lp, lp_coef;               /* noise brightness, Q16 */
    uint32_t attack;
    uint32_t inv_attack, inv_half, inv_rel;   /* 32767 / span, Q16: no division per sample */
    int32_t lin, dlin;                 /* decay ramp, Q24 */
    uint8_t wave, env;
    int16_t amp;
};
static struct sfx_voice sfx_voices[SFX_NOTES];
static int sfx_nvoices;
static uint32_t sfx_pos, sfx_total;
static uint32_t sfx_rng = 0x2545f491;
static int16_t sfx_buf[2][SFX_CHUNK * 2] __attribute__((aligned(16)));
static int sfx_buf_i;
static int16_t sfx_sine[1024];
static uint8_t sfx_playing_prio;
static uint32_t sfx_playing_until;
static int sfx_last = -1;              /* for the tests, and the repeat guard */
static uint32_t sfx_last_ms;
#define SFX_REPEAT_MS 60               /* the same sound again this soon is dropped */
/* A small room: two damped feedback delays of different lengths, one per
 * side, so a sound has somewhere to ring and the two ears differ. Its tail
 * plays on after the last note (SFX_TAIL_MS). */
#define SFX_TAIL_MS 320
#define SFX_ROOM_MAX 9000              /* 47 ms at 192 kHz */
static int16_t sfx_room[2][SFX_ROOM_MAX];
static int sfx_room_len[2], sfx_room_pos[2], sfx_room_send;
static int32_t sfx_room_lp[2];
static unsigned sfx_count;
/* A sound cut short by the next one leaves its level behind here, fading
 * out over about a millisecond, so the handover never jumps (a click). */
static int32_t sfx_declick, sfx_dry_last;

/* The polynomial step correction for a phase t in 0..65535, step dt. */
static int sfx_blep(int32_t t, int32_t dt)
{
    if (t < dt)
    {
        int32_t x = t * 32768 / dt;                    /* 0..1 in Q15 */
        return (int)((int64_t)(2 * x - (x * x >> 15) - 32768) * 26000 >> 15);
    }
    if (t > 65536 - dt)
    {
        int32_t x = (t - 65536) * 32768 / dt;          /* -1..0 in Q15 */
        return (int)((int64_t)((x * x >> 15) + 2 * x + 32768) * 26000 >> 15);
    }
    return 0;
}

static int sfx_osc(struct sfx_voice *v)
{
    int s;
    switch (v->wave)
    {
        case W_SINE: s = sfx_sine[v->phase >> 22]; break;
        case W_TRI:
        {
            int p = (int)(v->phase >> 16);
            s = p < 32768 ? p * 2 - 32768 : (65535 - p) * 2 - 32768;
            break;
        }
        case W_SQUARE:
        {
            /* PolyBLEP: round off each edge over one sample, so high
             * chip notes do not fold back as harsh aliases. */
            int32_t t = (int32_t)(v->phase >> 16), dt = (int32_t)(v->step >> 16) + 1;
            s = t < 32768 ? 26000 : -26000;
            s += sfx_blep(t, dt);
            s -= sfx_blep((t + 32768) & 65535, dt);
            break;
        }
        case W_NOISE:
            sfx_rng = sfx_rng * 1664525u + 1013904223u;
            v->lp += (int32_t)(((int64_t)((int16_t)(sfx_rng >> 16)) - v->lp) * v->lp_coef >> 16);
            s = v->lp * 2;
            if (s > 32767) s = 32767;
            if (s < -32767) s = -32767;
            return s;
        default:                       /* bell: a sine and an inharmonic partial */
            s = (sfx_sine[v->phase >> 22] * 2 + sfx_sine[v->phase2 >> 22]) / 3;
            v->phase2 += v->step / 100 * 276;
            break;
    }
    v->phase += v->step;
    v->step += v->dstep;
    return s;
}

/* x * 32767 / span, with inv = (32767 << 16) / span worked out once per
 * note: the mixer runs this for every sample of every voice, and a 64-bit
 * division is a slow library call on the R1's MIPS CPU. */
static inline int sfx_ramp(uint32_t x, uint32_t inv)
{
    uint32_t e = (uint32_t)(((uint64_t)x * inv) >> 16);
    return e > 32767 ? 32767 : (int)e;
}

/* 0..32767 */
static int sfx_envelope(struct sfx_voice *v)
{
    uint32_t k = v->k;
    if (v->env == E_SWELL)
    {
        uint32_t half = v->len / 2 ? v->len / 2 : 1;
        return sfx_ramp(k < half ? k : v->len - k, v->inv_half);
    }
    if (k < v->attack) return sfx_ramp(k, v->inv_attack);
    if (v->env == E_HOLD)
    {
        uint32_t rel = v->len / 6, left = v->len - k;
        return left >= rel || !rel ? 32767 : sfx_ramp(left, v->inv_rel);
    }
    v->lin -= v->dlin;
    if (v->lin < 0) v->lin = 0;
    int l = v->lin >> 9;               /* Q15 */
    return l * l >> 15;
}

static void sfx_render(int16_t *out, int frames)
{
    /* Q15, on a squared curve so the slider sounds even; 70 % is the old Normal */
    int pct = sfx_volume_pct();
    int32_t gain = 30 * 1024 * pct * pct / 10000;
    for (int i = 0; i < frames; i++, sfx_pos++)
    {
        int32_t mix = 0;
        for (int n = 0; n < sfx_nvoices; n++)
        {
            struct sfx_voice *v = &sfx_voices[n];
            if (sfx_pos < v->start || v->k >= v->len) continue;
            int e = sfx_envelope(v);
            mix += (int32_t)((int64_t)sfx_osc(v) * e >> 15) * v->amp >> 7;
            v->k++;
        }
        mix = (int32_t)((int64_t)mix * gain >> 15);
        if (sfx_declick)
        {
            mix += sfx_declick;
            sfx_declick = sfx_declick * 47 / 48;
        }
        sfx_dry_last = mix;
        int32_t side[2];
        for (int c = 0; c < 2; c++)
        {
            int j = sfx_room_pos[c];
            if (++sfx_room_pos[c] >= sfx_room_len[c]) sfx_room_pos[c] = 0;
            int32_t echo = sfx_room[c][j];
            /* the walls take the top off each reflection */
            sfx_room_lp[c] += (echo - sfx_room_lp[c]) >> 1;
            int32_t in = (mix * sfx_room_send >> 8) + (sfx_room_lp[c] * 5 >> 4);
            sfx_room[c][j] = (int16_t)(in > 32767 ? 32767 : in < -32767 ? -32767 : in);
            side[c] = mix + (echo * 9 >> 4);
        }
        for (int c = 0; c < 2; c++)
        {
            int32_t o = side[c];
            /* a soft knee from -3 dBFS at 4:1, so a stacked chord rounds
             * off instead of flattening against the ceiling */
            if (o > 23000) o = 23000 + (o - 23000) / 4;
            if (o < -23000) o = -23000 + (o + 23000) / 4;
            if (o > 32767) o = 32767;
            if (o < -32767) o = -32767;
            *out++ = (int16_t)o;
        }
    }
}

static void sfx_get_more(const void **start, size_t *size)
{
    if (sfx_pos >= sfx_total) return;
    int frames = sfx_total - sfx_pos < SFX_CHUNK ? (int)(sfx_total - sfx_pos) : SFX_CHUNK;
    int16_t *buf = sfx_buf[sfx_buf_i ^= 1];
    sfx_render(buf, frames);
    *start = buf;
    *size = frames * 2 * sizeof(int16_t);
}

static int sfx_volume_pct(void)
{
    int v = global_settings.sloppy_sfx_volume;
    return v < 0 ? 0 : v > 100 ? 100 : v;
}
static bool sfx_enabled(void) { return sfx_volume_pct() > 0; }

/* Interface sounds obey Settings > Sounds > Interface sounds; so do the
 * power sounds, though a shutdown outranks a tap. */
static bool sfx_is_interface(enum sfx_id id)
{
    return sfx_defs[id].prio == P_UI || id >= SFX_SYS_BOOT;
}

/* Play a sound with its pitch scaled by pitch_pct (100 as written) and its
 * level by vol_pct. Returns whether it plays. */
static bool sfx_play_ex(enum sfx_id id, int pitch_pct, int vol_pct)
{
    if (!sfx_enabled() || (unsigned)id >= SFX_COUNT || vol_pct <= 0) return false;
    const struct sfx_def *d = &sfx_defs[id];
    if (sfx_is_interface(id) && !global_settings.sloppy_ui_sounds) return false;
    uint32_t now = sl_ms();
    if (d->prio < sfx_playing_prio && (int32_t)(now - sfx_playing_until) < 0) return false;
    /* a fast scrub or a held key purrs instead of machine-gunning */
    if ((int)id == sfx_last && now - sfx_last_ms < SFX_REPEAT_MS) return false;
    if (!sfx_sine[256])
        for (int i = 0; i < 1024; i++)
            sfx_sine[i] = (int16_t)(sinf(i * 6.2831853f / 1024) * 32767);
    /* A sound of ours still playing hands over without stopping the
     * channel: the voices change under the PCM lock, never while the mixer
     * is rendering them, and the old level fades out (sfx_declick). The
     * channel is only restarted when it is idle or a keyclick has it. */
    pcm_play_lock();
    int left_frames;
    const int16_t *cur = mixer_channel_get_buffer(PCM_MIXER_CHAN_BEEP, &left_frames);
    bool running = mixer_channel_status(PCM_MIXER_CHAN_BEEP) == CHANNEL_PLAYING &&
                   cur >= sfx_buf[0] && cur < sfx_buf[0] + sizeof sfx_buf / sizeof sfx_buf[0][0];
    if (!running)
    {
        pcm_play_unlock();
        mixer_channel_stop(PCM_MIXER_CHAN_BEEP);
        pcm_play_lock();
        sfx_declick = 0;
    }
    else
        sfx_declick = sfx_dry_last;
    uint32_t fs = mixer_get_frequency();
    if (fs < 8000) fs = 44100;
    int left = (int)(fs * 37 / 1000), right = (int)(fs * 47 / 1000);
    if (right > SFX_ROOM_MAX) { left = SFX_ROOM_MAX * 37 / 47; right = SFX_ROOM_MAX; }
    if (left != sfx_room_len[0])       /* a new rate: start with an empty room */
    {
        memset(sfx_room, 0, sizeof sfx_room);
        sfx_room_lp[0] = sfx_room_lp[1] = 0;
        sfx_room_pos[0] = sfx_room_pos[1] = 0;
        sfx_room_len[0] = left; sfx_room_len[1] = right;
    }
    /* clicks stay close; chimes and fanfares get a little more room */
    sfx_room_send = d->prio == P_RESULT ? 90 : d->prio == P_NORMAL ? 70 : 50;
    uint32_t total_ms = 0;
    sfx_nvoices = 0;
    for (int i = 0; i < SFX_NOTES && d->n[i].dur; i++)
    {
        const struct sfx_note *n = &d->n[i];
        struct sfx_voice *v = &sfx_voices[sfx_nvoices++];
        memset(v, 0, sizeof *v);
        v->wave = n->wave;
        v->env = n->env;
        v->start = (uint32_t)((uint64_t)n->at * fs / 1000);
        v->len = (uint32_t)((uint64_t)n->dur * fs / 1000);
        if (!v->len) v->len = 1;
        v->attack = fs / 250 < v->len / 4 ? fs / 250 : v->len / 4 + 1;
        v->inv_attack = (32767u << 16) / v->attack;
        v->inv_half = (32767u << 16) / (v->len / 2 ? v->len / 2 : 1);
        v->inv_rel = (32767u << 16) / (v->len / 6 ? v->len / 6 : 1);
        v->lin = 1 << 24;
        v->dlin = (1 << 24) / (int32_t)(v->len - v->attack + 1);
        v->amp = (int16_t)(n->vol * vol_pct / 100);
        uint32_t f0 = n->f0, f1 = n->f1;
        if (n->wave != W_NOISE) { f0 = f0 * pitch_pct / 100; f1 = f1 * pitch_pct / 100; }
        uint64_t s0 = ((uint64_t)f0 << 32) / fs, s1 = ((uint64_t)f1 << 32) / fs;
        v->step = (uint32_t)s0;
        v->dstep = (int32_t)(((int64_t)s1 - (int64_t)s0) / (int64_t)v->len);
        /* one-pole lowpass: coef = w / (1 + w), w = 2 pi f / fs */
        float w = 6.2831853f * f0 / fs;
        v->lp_coef = (int32_t)(w / (1 + w) * 65536);
        if (n->at + n->dur > total_ms) total_ms = n->at + n->dur;
    }
    sfx_pos = 0;
    sfx_total = (uint32_t)((uint64_t)(total_ms + SFX_TAIL_MS) * fs / 1000);
    sfx_playing_prio = d->prio;
    sfx_playing_until = now + total_ms;
    sfx_last = id;
    sfx_last_ms = now;
    sfx_count++;
    pcm_play_unlock();
    if (d->prio != P_UI)               /* the music does not dip for a click */
        beep_duck(total_ms, d->prio == P_RESULT ? global_settings.sound_duck_cue :
                            global_settings.sound_duck_click);
    if (running) return true;          /* the mixer asks for the new sound next */
    const void *start = NULL;
    size_t size = 0;
    sfx_get_more(&start, &size);
    mixer_channel_set_amplitude(PCM_MIXER_CHAN_BEEP, MIX_AMP_UNITY);
    mixer_channel_play_data(PCM_MIXER_CHAN_BEEP, sfx_get_more, start, size);
    return true;
}

static bool sfx_play(enum sfx_id id) { return sfx_play_ex(id, 100, 100); }

/* A little variety for sounds that repeat a lot (taps, bounces). */
static void sfx_play_vary(enum sfx_id id)
{
    sfx_rng = sfx_rng * 1664525u + 1013904223u;
    sfx_play_ex(id, 94 + (int)((sfx_rng >> 24) % 13), 100);
}
