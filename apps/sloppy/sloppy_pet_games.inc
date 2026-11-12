/* Pixel-art arcade mini-games for the pocket pet: Catch Bug, Find the Ball,
 * Melody Gems and the classic Left or Right. Everything is drawn on one LCD
 * canvas at 30fps; the pet takes part in every game. GPLv2+. */
#define PET_GAME_W 128
#define PET_GAME_H 136
#define PET_CATCH_MS 20000
#define PET_CUP_SWAPS 6
#define PET_CUP_SHUFFLE_AT 1900
#define PET_CUP_SWAP_MS 440
#define PET_CUP_READY_AT (PET_CUP_SHUFFLE_AT + PET_CUP_SWAPS * PET_CUP_SWAP_MS)
#define PET_MEM_STEP 620
#define PET_MEM_LEAD 500
#define PET_HEAD_ROUNDS 5

static lv_obj_t *pet_game_page_obj, *pet_game_message, *pet_game_score;
static lv_obj_t *pet_game_canvas, *pet_game_float, *pet_game_again;
static lv_obj_t *pet_game_pads[2];
static lv_timer_t *pet_game_timer;
static uint16_t pet_game_buf[PET_GAME_W * PET_GAME_H * 9];
static enum pet_game_kind pet_game_kind;
static int pet_game_target, pet_game_hits, pet_game_misses, pet_game_input;
static uint8_t pet_game_pattern[5];
static uint32_t pet_game_started, pet_game_float_until;
static unsigned pet_game_t;
static bool pet_game_finished, pet_game_won;
static int pet_game_fun_gain;

/* Shared effect: what happened last, where and when. */
enum pet_game_fx { PET_FX_NONE, PET_FX_CATCH, PET_FX_MISS, PET_FX_CHEER, PET_FX_SHRUG };
static enum pet_game_fx pet_game_fx;
static int pet_game_fx_x, pet_game_fx_y;
static uint32_t pet_game_fx_at;

/* Catch Bug. Positions are 1/16 pixel fixed point. */
static int pet_bug_x, pet_bug_y, pet_bug_vx, pet_bug_vy, pet_bug_speed;
static int pet_bug_trail[6][2];
static uint32_t pet_bug_turn_at, pet_bug_last;
static int pet_catcher_x;

/* Find the Ball: pet_cup_pos[cup] is the table slot the cup sits on. */
static uint8_t pet_cup_pos[3];
static uint8_t pet_cup_swaps[PET_CUP_SWAPS][2];
static int pet_cup_swooshed;            /* swaps that have made their sound */
static int pet_cup_chosen = -1;
static uint32_t pet_cup_reveal_at;

/* Melody Gems. */
static int pet_mem_round;
static bool pet_mem_demo;
static uint32_t pet_mem_phase_at, pet_mem_lit_until;
static int pet_mem_lit = -1;

/* Left or Right. */
static int pet_head_round, pet_head_dir, pet_head_guess, pet_head_score;
static int8_t pet_head_results[PET_HEAD_ROUNDS];
static uint32_t pet_head_turn_at;

/* Dance With Me: tap on the beat of the song that is playing. */
#define PET_RHY_NOTES 16
#define PET_RHY_LEAD 3                  /* beats a note takes to cross the track */
#define PET_RHY_PERFECT 90
#define PET_RHY_GOOD 180
#define PET_RHY_RING 20
static uint32_t pet_rhy_first, pet_rhy_judge_at;
static int pet_rhy_interval, pet_rhy_perfect, pet_rhy_good, pet_rhy_combo, pet_rhy_best;
static int pet_rhy_count, pet_rhy_last;   /* count-in shown, last judgement */
static uint8_t pet_rhy_judged[PET_RHY_NOTES]; /* 0 waiting, 1 perfect, 2 good, 3 miss */

static const char *const pet_gem_star[12] = {
    "......#......", ".....#+#.....", "....#+++#....", "#####+++#####",
    "#+++++++++++#", ".#+++++++++#.", "..#+++++++#..", "..#+++#+++#..",
    ".#+++#.#+++#.", ".#++#...#++#.", "#++#.....#++#", "###.......###" };
static const char *const pet_gem_heart[12] = {
    "..###...###..", ".#+++#.#+++#.", "#++o++#+++++#", "#+o++++++++o#",
    "#+++++++++++#", ".#+++++++++#.", "..#+++++++#..", "...#+++++#...",
    "....#+++#....", ".....#+#.....", "......#......", "............." };
static const char *const pet_gem_diamond[12] = {
    "...#######...", "..#++#+#++#..", ".#++#+++#++#.", "#############",
    ".#++#+++#++#.", "..#++#+#++#..", "...#+#+#+#...", "....#+++#....",
    ".....#+#.....", "......#......", ".............", "............." };
static const char *const pet_gem_moon[12] = {
    "....#####....", "..##+++##....", ".#+++##......", ".#++#........",
    "#++#.........", "#++#.........", "#++#.........", ".#++#........",
    ".#+++##......", "..##+++##....", "....#####....", "............." };
static const char *const *const pet_gems[4] = {
    pet_gem_star, pet_gem_heart, pet_gem_diamond, pet_gem_moon };

static void pet_game_show_float(const char *label, int lx, int ly, uint32_t ms)
{
    if (!pet_game_float) return;
    lv_label_set_text(pet_game_float, label);
    lv_obj_update_layout(pet_game_float);
    int w = lv_obj_get_width(pet_game_float);
    int px = lx * 3 - w / 2 + 10, py = ly * 3 - 18;
    if (px < 14) px = 14;
    if (px + w > PET_GAME_W * 3 + 6) px = PET_GAME_W * 3 + 6 - w;
    if (py < 14) py = 14;
    lv_obj_set_pos(pet_game_float, px, py);
    lv_obj_remove_flag(pet_game_float, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(pet_game_float);
    pet_game_float_until = pet_now() + ms;
}

static void pet_game_set_score(void)
{
    if (!pet_game_score) return;
    char score[48];
    switch (pet_game_kind) {
    case PET_CATCH:
        snprintf(score, sizeof score, "CATCH %d / 5   MISSES %d / 3", pet_game_hits, pet_game_misses);
        break;
    case PET_MEMORY:
        snprintf(score, sizeof score, "ROUND %d / 3   STEP %d / %d",
                 pet_mem_round + 1, pet_game_input, 3 + pet_mem_round);
        break;
    case PET_CUPS:
        snprintf(score, sizeof score, "THREE CUPS, ONE PEARL");
        break;
    case PET_RHYTHM:
        snprintf(score, sizeof score, "PERFECT %d   GOOD %d   COMBO %d",
                 pet_rhy_perfect, pet_rhy_good, pet_rhy_combo);
        break;
    case PET_HEADING:
        snprintf(score, sizeof score, "ROUND %d / %d   RIGHT GUESSES %d",
                 pet_head_round < PET_HEAD_ROUNDS ? pet_head_round + 1 : PET_HEAD_ROUNDS,
                 PET_HEAD_ROUNDS, pet_head_score);
        break;
    }
    lv_label_set_text(pet_game_score, score);
}

static void pet_game_finish(bool won)
{
    if (pet_game_finished) return;
    pet_game_finished = true;
    pet_game_won = won;
    int joy_before = pet.joy;
    sl_pet_game_result(&pet, won);
    sfx_play(pet.dead ? SFX_PET_RIP : won ? SFX_PET_WIN : SFX_PET_LOSE);
    pet_game_fun_gain = (int)pet.joy - joy_before;
    pet_save();
    pet_motion = pet.manic ? PET_MANIC : won ? PET_CELEBRATE : PET_IDLE;
    pet_motion_until = sl_ms() + 1700;
    if (pet_game_message)
        lv_label_set_text(pet_game_message, pet.dead ? "YOUR PET COLLAPSED FROM EXHAUSTION" :
                          pet.crisis ? "OVERWHELMED! RUNNING ON EMPTY" :
                          pet.manic ? "SO WOUND UP! ENERGY IS DRAINING" :
                          won ? "WELL DONE! FUN IS UP" : "GOOD TRY! PLAY AGAIN");
    char gain[24];
    snprintf(gain, sizeof gain, "+%d FUN!", pet_game_fun_gain);
    if (!pet.dead && (pet_game_kind != PET_CUPS || won))
        pet_game_show_float(pet_game_fun_gain > 0 ? gain : won ? "YOU WIN!" : "NICE TRY",
                            PET_GAME_W / 2, pet_game_kind == PET_HEADING ? 118 : 58, 1800);
    for (int i = 0; i < 2; i++) if (pet_game_pads[i])
        lv_obj_add_state(pet_game_pads[i], LV_STATE_DISABLED);
    if (pet_game_again && !pet.dead) lv_obj_remove_flag(pet_game_again, LV_OBJ_FLAG_HIDDEN);
    if (pet.dead) pet_game_show_float("R.I.P.", PET_GAME_W / 2, 58, 4000);
    pet_refresh();
}

/* ------------------------------------------------------------ drawing */
static void pet_game_ground(int y)
{
    pet_px(0, y, PET_GAME_W, 2, 1);
    for (int x = 3; x < PET_GAME_W; x += 9) pet_px(x, y + 3 + (x % 2), 2, 1, 2);
}

static void pet_game_x(int x, int y, int s, int ink)
{
    for (int i = 0; i < s; i++) { pet_px(x + i, y + i, 1, 1, ink); pet_px(x + s - 1 - i, y + i, 1, 1, ink); }
}

static void pet_game_bug(int x, int y, unsigned t, int ink)
{
    bool up = (t / 2) & 1;
    /* Fluttering wings, striped body, head and twitching antennae. */
    if (up) { pet_disc(x - 4, y - 3, 3, 1); pet_disc(x - 4, y - 3, 2, 3);
              pet_disc(x + 4, y - 3, 3, 1); pet_disc(x + 4, y - 3, 2, 3); }
    else    { pet_disc(x - 5, y, 3, 1); pet_disc(x - 5, y, 2, 3);
              pet_disc(x + 5, y, 3, 1); pet_disc(x + 5, y, 2, 3); }
    pet_disc(x, y + 1, 3, ink); pet_px(x - 2, y + 1, 5, 1, 2); pet_px(x - 1, y + 3, 3, 1, 2);
    pet_disc(x, y - 4, 2, ink);
    int twitch = (t / 3) & 1;
    pet_line(x - 1, y - 5, x - 3 - twitch, y - 8, ink);
    pet_line(x + 1, y - 5, x + 3 + twitch, y - 8, ink);
    pet_px(x - 4, y + 3, 2, 1, ink); pet_px(x + 3, y + 3, 2, 1, ink);
}

static void pet_game_bug_icon(int x, int y, bool filled)
{
    pet_px(x + 1, y, 1, 1, 1); pet_px(x + 5, y, 1, 1, 1);
    pet_px(x + 2, y + 1, 3, 1, 1);
    pet_px(x, y + 2, 7, 3, filled ? 1 : 2);
    if (!filled) pet_px(x + 1, y + 3, 5, 1, 0);
    pet_px(x + 2, y + 5, 3, 1, filled ? 1 : 2);
}

static void pet_game_cup(int cx, int bottom, bool ghost)
{
    int top = bottom - 24;
    for (int r = 0; r < 24; r++)
    {
        int half = 7 + r * 6 / 23;
        if (ghost)
        {
            if (r % 3 == 0) { pet_px(cx - half, top + r, 1, 1, 2); pet_px(cx + half, top + r, 1, 1, 2); }
            continue;
        }
        pet_px(cx - half, top + r, half * 2 + 1, 1, 1);
        if (r > 0 && r < 19)
        {
            pet_px(cx - half + 1, top + r, half * 2 - 1, 1, 2);
            pet_px(cx - half + 3, top + r, 2, 1, 3);
            pet_px(cx + half - 3, top + r, 1, 1, 1);
        }
        if (r == 21) pet_px(cx - half + 2, top + r, half * 2 - 3, 1, 3);
    }
    if (!ghost) { pet_px(cx - 3, top - 3, 7, 3, 1); pet_px(cx - 2, top - 2, 5, 1, 3); }
}

static void pet_game_pearl(int x, int y, unsigned t, bool shine)
{
    pet_disc(x, y, 4, 1); pet_disc(x, y, 3, 3); pet_px(x - 2, y - 2, 2, 2, 0);
    if (shine)
    {
        int r = 7 + (int)(t % 4);
        pet_spark(x - r, y - 4, 2); pet_spark(x + r, y - 5, 2); pet_spark(x, y - r - 2, 2);
    }
}

static void pet_game_star_burst(int x, int y, int r)
{
    static const int8_t dir[8][2] = { {0,-1}, {1,-1}, {1,0}, {1,1}, {0,1}, {-1,1}, {-1,0}, {-1,-1} };
    for (int i = 0; i < 8; i++)
    {
        int len = i % 2 ? r * 2 / 3 : r;
        pet_line(x + dir[i][0] * (r / 3), y + dir[i][1] * (r / 3),
                 x + dir[i][0] * len, y + dir[i][1] * len, 1);
    }
}

static void pet_game_puff(int x, int y, uint32_t age)
{
    int r = 2 + (int)age / 60;
    for (int i = 0; i < 6; i++)
    {
        static const int8_t d[6][2] = { {-2,0}, {2,0}, {-1,-1}, {1,-1}, {-2,-1}, {2,-1} };
        pet_disc(x + d[i][0] * r, y + d[i][1] * r, 2 - (int)age / 200, 2);
    }
}

static void pet_mem_tile(int index, int *x, int *y)
{
    *x = 8 + (index % 2) * 58;
    *y = 14 + (index / 2) * 40;
}

/* --------------------------------------------------------------- catch */
static void pet_bug_respawn(void)
{
    pet_bug_x = (14 + (int)pet_rand(100)) * 16;
    pet_bug_y = (24 + (int)pet_rand(20)) * 16;
    for (int i = 0; i < 6; i++) { pet_bug_trail[i][0] = pet_bug_x / 16; pet_bug_trail[i][1] = pet_bug_y / 16; }
    pet_bug_turn_at = 0;
}

static void pet_bug_step(uint32_t now)
{
    static const int8_t dirs[16][2] = {
        {16,0}, {15,6}, {11,11}, {6,15}, {0,16}, {-6,15}, {-11,11}, {-15,6},
        {-16,0}, {-15,-6}, {-11,-11}, {-6,-15}, {0,-16}, {6,-15}, {11,-11}, {15,-6} };
    if (!pet_bug_last) pet_bug_last = now;
    int steps = (int)(now - pet_bug_last) / 33;
    if (steps > 12) steps = 12;
    pet_bug_last += steps * 33;
    for (int s = 0; s < steps; s++)
    {
        if ((int32_t)(now - pet_bug_turn_at) >= 0)
        {
            int d = (int)pet_rand(16);
            pet_bug_vx = dirs[d][0] * pet_bug_speed / 16;
            pet_bug_vy = dirs[d][1] * pet_bug_speed / 16;
            pet_bug_turn_at = now + 220 + pet_rand(380);
        }
        pet_bug_x += pet_bug_vx; pet_bug_y += pet_bug_vy;
        /* Erratic buzz: a small wobble on top of the heading. */
        pet_bug_y += ((pet_game_t + s) % 4 < 2 ? 6 : -6);
        if (pet_bug_x < 10 * 16) { pet_bug_x = 10 * 16; pet_bug_vx = abs(pet_bug_vx); }
        if (pet_bug_x > 118 * 16) { pet_bug_x = 118 * 16; pet_bug_vx = -abs(pet_bug_vx); }
        if (pet_bug_y < 26 * 16) { pet_bug_y = 26 * 16; pet_bug_vy = abs(pet_bug_vy); }
        if (pet_bug_y > 88 * 16) { pet_bug_y = 88 * 16; pet_bug_vy = -abs(pet_bug_vy); }
        memmove(&pet_bug_trail[1], &pet_bug_trail[0], sizeof pet_bug_trail - sizeof pet_bug_trail[0]);
        pet_bug_trail[0][0] = pet_bug_x / 16; pet_bug_trail[0][1] = pet_bug_y / 16;
    }
    int goal = pet_bug_x / 16 - 16;
    if (goal < 0) goal = 0;
    if (goal > PET_GAME_W - 34) goal = PET_GAME_W - 34;
    if (pet_catcher_x < goal) pet_catcher_x += pet_catcher_x + 2 < goal ? 2 : 1;
    else if (pet_catcher_x > goal) pet_catcher_x -= pet_catcher_x - 2 > goal ? 2 : 1;
}

static void pet_catch_render(uint32_t now, uint32_t elapsed)
{
    unsigned t = pet_game_t;
    /* Arcade HUD: countdown bar, caught bugs and misses. */
    int left = elapsed >= PET_CATCH_MS ? 0 : (int)(PET_CATCH_MS - elapsed);
    pet_px(2, 2, 124, 6, 1); pet_px(3, 3, 122, 4, 0);
    bool hurry = left < 5000 && (t / 6) % 2;
    if (!hurry) pet_px(3, 3, 122 * left / PET_CATCH_MS, 4, left < 5000 ? 1 : 2);
    for (int k = 0; k < 5; k++) pet_game_bug_icon(3 + k * 10, 10, k < pet_game_hits);
    for (int k = 0; k < 3; k++) pet_game_x(96 + k * 10, 10, 6, k < pet_game_misses ? 1 : 2);
    pet_px(0, 18, PET_GAME_W, 1, 2);
    /* Meadow: grass and two flowers under the chase. */
    pet_game_ground(132);
    for (int x = 4; x < PET_GAME_W; x += 12) { pet_px(x, 128, 1, 4, 2); pet_px(x + 2, 129, 1, 3, 2); }
    pet_disc(12, 122, 2, 2); pet_px(12, 124, 1, 8, 1);
    pet_disc(118, 120, 2, 2); pet_px(118, 122, 1, 10, 1);
    int bx = pet_bug_x / 16, by = pet_bug_y / 16;
    if (!pet_game_finished)
    {
        for (int i = 2; i < 6; i += 2) pet_px(pet_bug_trail[i][0], pet_bug_trail[i][1] + 2, 1, 1, 2);
        pet_game_bug(bx, by, t, 1);
    }
    uint32_t fx_age = now - pet_game_fx_at;
    if (pet_game_fx == PET_FX_CATCH && fx_age < 520)
    {
        pet_game_star_burst(pet_game_fx_x, pet_game_fx_y, 6 + (int)fx_age / 40);
        for (int i = 0; i < 3 && fx_age < 200; i++)
            pet_line(pet_game_fx_x - 9 + i * 4, pet_game_fx_y + 9, pet_game_fx_x - 3 + i * 4, pet_game_fx_y - 9, 1);
        pet_game_bug(pet_game_fx_x, pet_game_fx_y - (int)fx_age / 25, 0, 2);
    }
    else if (pet_game_fx == PET_FX_MISS && fx_age < 400)
    {
        pet_game_puff(pet_game_fx_x, pet_game_fx_y, fx_age);
        pet_game_x(pet_game_fx_x - 3, pet_game_fx_y - 3, 7, 1);
    }
    /* The pet chases along the ground, lunging on every swipe. */
    bool cheer = (pet_game_fx == PET_FX_CATCH && fx_age < 600) || (pet_game_finished && pet_game_won);
    bool lunge = pet_game_fx != PET_FX_NONE && fx_age < 260;
    int look = bx > pet_catcher_x + 26 ? 2 : bx < pet_catcher_x + 6 ? -2 : 0;
    int py = 100 - (lunge ? 7 : 0) - (cheer && (t / 3) % 2 ? 4 : 0);
    pet_actor_ex(pet_catcher_x, py, cheer ? PET_CELEBRATE : PET_PLAYING, t / 2, look,
                 SL_PET_GROWN, cheer);
    if (lunge) pet_line(pet_catcher_x + 16, py - 2, pet_game_fx_x, pet_game_fx_y + 4, 2);
}

static void pet_catch_tap(int lx, int ly)
{
    uint32_t now = pet_now();
    int dx = lx - pet_bug_x / 16, dy = ly - pet_bug_y / 16;
    pet_game_fx_at = now;
    if (dx * dx + dy * dy <= 13 * 13)
    {
        pet_game_hits++;
        pet_game_fx = PET_FX_CATCH;
        pet_game_fx_x = pet_bug_x / 16; pet_game_fx_y = pet_bug_y / 16;
        pet_game_show_float("CAUGHT!", pet_game_fx_x, pet_game_fx_y - 22, 650);
        sfx_play_ex(SFX_PET_CATCH, 100 + pet_game_hits * 8, 100);
        pet_bug_speed += 4;
        pet_bug_respawn();
    }
    else
    {
        pet_game_misses++;
        pet_game_fx = PET_FX_MISS;
        sfx_play(SFX_PET_MISS);
        pet_game_fx_x = lx; pet_game_fx_y = ly;
    }
    pet_game_set_score();
    if (pet_game_hits >= 5 || pet_game_misses >= 3) pet_game_finish(pet_game_hits >= 5);
}

/* ---------------------------------------------------------------- cups */
static int pet_cup_slot_x(int slot) { return 24 + slot * 40; }

static void pet_cup_positions(uint32_t elapsed, int cx[3], int lift[3], int ghost[3])
{
    uint8_t pos[3] = { 0, 1, 2 };
    for (int i = 0; i < 3; i++) { lift[i] = 0; ghost[i] = -999; }
    if (elapsed < 1400) for (int i = 0; i < 3; i++) lift[i] = 30;
    else if (elapsed < PET_CUP_SHUFFLE_AT)
        for (int i = 0; i < 3; i++) lift[i] = 30 - (int)(elapsed - 1400) * 30 / 500;
    int done = elapsed < PET_CUP_SHUFFLE_AT ? 0 :
               (int)(elapsed - PET_CUP_SHUFFLE_AT) / PET_CUP_SWAP_MS;
    if (done > PET_CUP_SWAPS) done = PET_CUP_SWAPS;
    for (int s = 0; s < done; s++)
    {
        uint8_t a = pet_cup_swaps[s][0], b = pet_cup_swaps[s][1], keep = pos[a];
        pos[a] = pos[b]; pos[b] = keep;
    }
    for (int i = 0; i < 3; i++) cx[i] = pet_cup_slot_x(pos[i]);
    if (done < PET_CUP_SWAPS && elapsed >= PET_CUP_SHUFFLE_AT)
    {
        int a = pet_cup_swaps[done][0], b = pet_cup_swaps[done][1];
        int p = (int)((elapsed - PET_CUP_SHUFFLE_AT) % PET_CUP_SWAP_MS) * 1000 / PET_CUP_SWAP_MS;
        int ax = pet_cup_slot_x(pos[a]), bx = pet_cup_slot_x(pos[b]);
        ghost[a] = cx[a] + (bx - ax) * (p > 250 ? p - 250 : 0) / 1000;
        ghost[b] = cx[b] + (ax - bx) * (p > 250 ? p - 250 : 0) / 1000;
        cx[a] = ax + (bx - ax) * p / 1000;
        cx[b] = bx + (ax - bx) * p / 1000;
        int arc = p * (1000 - p) / 25000;
        lift[a] = arc;
        lift[b] = -arc / 3;
    }
    for (int i = 0; i < 3; i++) pet_cup_pos[i] = pos[i];
}

static void pet_cups_render(uint32_t now, uint32_t elapsed)
{
    unsigned t = pet_game_t;
    int cx[3], lift[3], ghost[3];
    pet_cup_positions(elapsed, cx, lift, ghost);
    /* Table with a checked cloth edge. */
    pet_px(0, 96, PET_GAME_W, 2, 1);
    for (int x = 0; x < PET_GAME_W; x += 8) pet_px(x + ((x / 8) % 2) * 4, 98, 4, 3, 2);
    pet_px(0, 101, PET_GAME_W, 1, 1);
    pet_px(8, 102, 3, 32, 1); pet_px(117, 102, 3, 32, 1);
    pet_game_ground(133);
    int target = pet_game_target;
    int ball_x = cx[target], ball_y = 91;
    if (elapsed < 1400)
    {
        /* The pearl bounces three times, each bounce lower. */
        int p = (int)elapsed % 467, h = 24 - (int)elapsed / 70;
        if (h < 0) h = 0;
        ball_y = 91 - h * 4 * p * (467 - p) / (467 * 467);
    }
    uint32_t reveal = pet_cup_chosen >= 0 ? now - pet_cup_reveal_at : 0;
    if (pet_cup_chosen >= 0)
    {
        int up = reveal >= 300 ? 24 : (int)reveal * 24 / 300;
        lift[pet_cup_chosen] = up;
        if (pet_cup_chosen != target && reveal > 700)
            lift[target] = reveal >= 1000 ? 24 : (int)(reveal - 700) * 24 / 300;
    }
    bool ball_visible = lift[target] >= 12 || elapsed < 1400;
    if (ball_visible)
        pet_game_pearl(ball_x, ball_y, t, pet_cup_chosen == target || elapsed < 1400);
    if (pet_cup_chosen >= 0 && pet_cup_chosen != target && reveal < 600)
        pet_game_puff(cx[pet_cup_chosen], 93, reveal);
    for (int i = 0; i < 3; i++)
    {
        if (ghost[i] != -999) pet_game_cup(ghost[i], 96 - lift[i], true);
        pet_game_cup(cx[i], 96 - lift[i], false);
        if (elapsed >= PET_CUP_READY_AT && pet_cup_chosen < 0 && !pet_game_finished && (t / 8) % 2)
            pet_px(cx[i] - 1, 60, 3, 3, 1);
    }
    /* The pet watches the shuffle from under the table's edge. */
    int pet_x = 48, py = 102;
    bool cheer = pet_cup_chosen >= 0 && pet_cup_chosen == target;
    bool shrug = pet_cup_chosen >= 0 && pet_cup_chosen != target;
    if (cheer && reveal > 200) py -= (int)((reveal / 60) % 6 < 3 ? 8 : 2);
    int look = 0;
    if (elapsed >= PET_CUP_SHUFFLE_AT && elapsed < PET_CUP_READY_AT)
        look = cx[target] < 50 ? -2 : cx[target] > 78 ? 2 : 0;
    pet_actor_ex(pet_x, py, cheer ? PET_CELEBRATE : shrug ? PET_WASHING : PET_IDLE,
                 t / 2, look, SL_PET_GROWN, cheer);
    if (shrug)
    {
        int qx = pet_x + 36, qy = py - 4;
        pet_px(qx, qy, 5, 1, 1); pet_px(qx + 4, qy + 1, 1, 2, 1);
        pet_px(qx + 2, qy + 3, 2, 1, 1); pet_px(qx + 2, qy + 4, 1, 2, 1);
        pet_px(qx + 2, qy + 7, 1, 1, 1);
    }
    if (cheer) { pet_spark(pet_x - 6, py + 2, 3); pet_spark(pet_x + 40, py, 3); }
    if (elapsed < 1400) pet_game_star_burst(ball_x, 60, 3);
}

static void pet_cups_tap(int lx, int ly)
{
    uint32_t elapsed = pet_now() - pet_game_started;
    if (elapsed < PET_CUP_READY_AT || pet_cup_chosen >= 0 || ly < 50 || ly > 112) return;
    int cx[3], lift[3], ghost[3];
    pet_cup_positions(elapsed, cx, lift, ghost);
    int best = -1, best_d = 18;
    for (int i = 0; i < 3; i++) if (abs(lx - cx[i]) < best_d) { best = i; best_d = abs(lx - cx[i]); }
    if (best < 0) return;
    pet_cup_chosen = best;
    pet_cup_reveal_at = pet_now();
    pet_game_finish(best == pet_game_target);
    if (best != pet_game_target) pet_game_show_float("EMPTY!", cx[best], 62, 900);
}

/* -------------------------------------------------------------- memory */
/* Star, Heart, Diamond, Moon: C, E, G and the C above, as percent of C5. */
static const uint8_t pet_gem_pitch[4] = { 100, 126, 150, 200 };
static int pet_mem_sung = -1;           /* demo beat whose note has played */
static void pet_mem_render(uint32_t now)
{
    unsigned t = pet_game_t;
    int len = 3 + pet_mem_round;
    int lit = -1, beat = -1;
    if (pet_mem_demo && !pet_game_finished)
    {
        int32_t since = (int32_t)(now - pet_mem_phase_at) - PET_MEM_LEAD;
        if (since >= 0)
        {
            beat = since / PET_MEM_STEP;
            if (beat < len && since % PET_MEM_STEP < 420) lit = pet_game_pattern[beat];
            if (beat < len && beat != pet_mem_sung)
            {
                pet_mem_sung = beat;
                sfx_play_ex(SFX_PET_GEM, pet_gem_pitch[pet_game_pattern[beat]], 100);
            }
            if (beat >= len)
            {
                pet_mem_demo = false;
                pet_game_input = 0;
                if (pet_game_message) lv_label_set_text(pet_game_message, "YOUR TURN! REPEAT THE GEMS");
                pet_game_set_score();
                beat = -1;
            }
        }
    }
    if ((int32_t)(now - pet_mem_lit_until) < 0) lit = pet_mem_lit;
    bool blink_answer = pet_game_finished && !pet_game_won && (t / 4) % 2;
    for (int i = 0; i < 4; i++)
    {
        int x, y;
        pet_mem_tile(i, &x, &y);
        bool on = i == lit || (blink_answer && i == pet_game_pattern[pet_game_input]);
        pet_px(x, y, 54, 36, 1);
        pet_px(x + 2, y + 2, 50, 32, on ? 1 : 0);
        if (!on) { pet_px(x + 2, y + 31, 50, 3, 2); pet_px(x + 3, y + 3, 6, 1, 3); }
        pet_glyph_s(pet_gems[i], 12, x + 14, y + 6, on ? 2 : 0, 2);
        if (on)
        {
            pet_spark(x - 3, y - 3, 2); pet_spark(x + 57, y - 3, 2);
            pet_spark(x - 3, y + 39, 2); pet_spark(x + 57, y + 39, 2);
            pet_px(x + 24, y - 4, 6, 2, 1); pet_px(x + 24, y + 38, 6, 2, 1);
        }
    }
    /* Beat dots show how long the melody is. */
    for (int k = 0; k < len; k++)
    {
        bool done = pet_mem_demo ? k <= beat : k < pet_game_input;
        pet_disc(64 - len * 4 + k * 8 + 3, 98, 2, done ? 1 : 2);
    }
    pet_game_ground(133);
    int pet_x = 48, py = 101;
    int lean = 0;
    enum pet_motion motion = PET_IDLE;
    if (lit >= 0)
    {
        motion = PET_CELEBRATE;
        pet_x += (lit % 2) ? 12 : -12;
        py -= lit < 2 ? 5 : 0;
        lean = (lit % 2) ? 2 : -2;
    }
    if (pet_game_finished && pet_game_won) { motion = PET_CELEBRATE; py -= (t / 3) % 2 ? 5 : 0; }
    pet_actor_ex(pet_x, py, motion, beat >= 0 ? (unsigned)beat * 2 + (t / 3) % 2 : t / 2, lean,
                 SL_PET_GROWN, lit >= 0 || (pet_game_finished && pet_game_won));
    if (lit >= 0)
    {
        /* Musical notes pop out on every beat. */
        int nx = pet_x + (lit % 2 ? 36 : -8), ny = py - 4 - (int)(t % 6);
        pet_disc(nx, ny + 6, 2, 1); pet_px(nx + 2, ny, 1, 6, 1); pet_px(nx + 2, ny, 3, 1, 1);
    }
}

static void pet_mem_press(int gem)
{
    if (pet_game_finished || pet_mem_demo || gem < 0 || gem > 3) return;
    uint32_t now = pet_now();
    pet_mem_lit = gem;
    pet_mem_lit_until = now + 260;
    if (gem != pet_game_pattern[pet_game_input]) { pet_game_finish(false); return; }
    sfx_play_ex(SFX_PET_GEM, pet_gem_pitch[gem], 100);
    pet_game_input++;
    int len = 3 + pet_mem_round;
    if (pet_game_input >= len)
    {
        if (pet_mem_round >= 2) { pet_game_set_score(); pet_game_finish(true); return; }
        pet_mem_round++;
        pet_mem_demo = true;
        pet_mem_sung = -1;
        pet_mem_phase_at = now + 450;
        pet_game_input = 0;
        char msg[40];
        snprintf(msg, sizeof msg, "ROUND %d! WATCH THE GEMS", pet_mem_round + 1);
        if (pet_game_message) lv_label_set_text(pet_game_message, msg);
        pet_game_show_float("NICE!", 64, 50, 600);
    }
    pet_game_set_score();
}

static void pet_mem_tap(int lx, int ly)
{
    for (int i = 0; i < 4; i++)
    {
        int x, y;
        pet_mem_tile(i, &x, &y);
        if (lx >= x - 2 && lx < x + 56 && ly >= y - 2 && ly < y + 38) { pet_mem_press(i); return; }
    }
}

/* ---------------------------------------------------- left or right */
static void pet_head_render(uint32_t now)
{
    /* Drawn at 6x: a big close-up of the pet for the classic game. */
    unsigned t = pet_game_t;
    pet_target(pet_game_buf, PET_GAME_W / 2, PET_GAME_H / 2, 6);
    pet_px(0, 0, PET_GAME_W / 2, PET_GAME_H / 2, 0);
    for (int k = 0; k < PET_HEAD_ROUNDS; k++)
    {
        int x = 12 + k * 10, y = 5;
        if (pet_head_results[k] > 0) pet_disc(x, y, 3, 1);
        else if (pet_head_results[k] < 0) pet_game_x(x - 2, y - 2, 5, 1);
        else
        {
            pet_disc(x, y, 3, 1); pet_disc(x, y, 2, 0);
            if (k == pet_head_round && (t / 6) % 2) pet_px(x, y, 1, 1, 1);
        }
    }
    pet_px(0, 62, 64, 1, 1);
    for (int x = 2; x < 64; x += 6) pet_px(x, 64, 2, 1, 2);
    int32_t since = pet_head_guess ? (int32_t)(now - pet_head_turn_at) : -1;
    int look = 0, sway = 0, py = 26;
    bool victory = pet_game_finished && pet_game_won;
    if (!pet_head_guess && !victory)
    {
        /* Anticipation: a wobble and teasing side glances. */
        sway = (t / 5) % 2 ? 1 : 0;
        look = (t / 12) % 4 == 1 ? -1 : (t / 12) % 4 == 3 ? 1 : 0;
        pet_px(29, 18, 1, 2, 1); pet_px(34, 18, 1, 2, 1);
    }
    else if (since < 220)
    {
        sway = (since / 40) % 2 ? 1 : -1;
    }
    else
    {
        look = pet_head_dir * 3;
        sway = pet_head_dir * 3;
        /* Whoosh lines trail the head turn. */
        for (int i = 0; i < 3; i++)
        {
            int wx = pet_head_dir > 0 ? 12 - i * 3 : 52 + i * 3;
            pet_px(wx, 30 + i * 5, 6, 1, 2);
        }
        int ax = pet_head_dir > 0 ? 54 : 4;
        pet_px(ax, 24, 6, 2, 1);
        pet_px(ax + (pet_head_dir > 0 ? 3 : 0), 22, 3, 6, 1);
        pet_px(ax + (pet_head_dir > 0 ? 5 : -1), 23, 2, 4, 1);
    }
    bool right = pet_head_guess && pet_head_guess == pet_head_dir;
    bool happy = (since >= 260 && right) || victory;
    if (happy) py -= (since / 90) % 2 ? 3 : 0;
    pet_actor_ex(16 + sway, py, happy ? PET_CELEBRATE : PET_IDLE, happy ? t / 2 : t / 4, look,
                 SL_PET_GROWN, happy);
    if (since >= 260 && !right)
    {
        /* A smug little tongue for a wrong guess. */
        pet_px(31 + look * 2, py + 23, 3, 3, 3);
        pet_px(31 + look * 2, py + 23, 3, 1, 1);
    }
}

static void pet_head_next(void)
{
    pet_head_guess = 0;
    pet_head_dir = pet_rand(2) ? 1 : -1;
    for (int i = 0; i < 2; i++) if (pet_game_pads[i])
        lv_obj_remove_state(pet_game_pads[i], LV_STATE_DISABLED);
    if (pet_game_message) lv_label_set_text(pet_game_message, "WHICH WAY WILL I LOOK?");
    pet_game_set_score();
}

static void pet_heading_guess(int dir)
{
    if (pet_game_finished || pet_head_guess || pet_head_round >= PET_HEAD_ROUNDS) return;
    pet_head_guess = dir < 0 ? -1 : 1;
    pet_head_turn_at = pet_now();
    bool right = pet_head_guess == pet_head_dir;
    pet_head_results[pet_head_round] = right ? 1 : -1;
    if (right) pet_head_score++;
    for (int i = 0; i < 2; i++) if (pet_game_pads[i])
        lv_obj_add_state(pet_game_pads[i], LV_STATE_DISABLED);
    if (pet_game_message) lv_label_set_text(pet_game_message,
        right ? "YES! YOU READ MY MIND" : pet_head_dir < 0 ? "NOPE! I LOOKED LEFT" : "NOPE! I LOOKED RIGHT");
    pet_game_show_float(right ? "YES!" : "NOPE!", pet_head_dir > 0 ? 96 : 32, 30, 800);
    sfx_play(right ? SFX_PET_YES : SFX_PET_NOPE);
    pet_game_set_score();
}

static void pet_head_step(uint32_t now)
{
    if (!pet_head_guess || pet_game_finished) return;
    if ((int32_t)(now - pet_head_turn_at) < 1150) return;
    pet_head_round++;
    if (pet_head_round >= PET_HEAD_ROUNDS)
    {
        pet_head_guess = 0;
        pet_game_set_score();
        pet_game_finish(pet_head_score >= 3);
    }
    else pet_head_next();
}

static void pet_heading_cb(lv_event_t *e)
{
    pet_heading_guess((int)(intptr_t)lv_event_get_user_data(e));
}

/* Notes slide along a track to the ring under the pet; the tempo comes from
 * the music's beat and is fixed for the round, so the track stays steady
 * even when the detector wavers. */
static uint32_t pet_rhy_hit(int i) { return pet_rhy_first + (uint32_t)(i * pet_rhy_interval); }

static void pet_rhy_start(uint32_t now)
{
    int interval = pet_mus.interval > 0 ? pet_mus.interval : 500;
    while (interval < 380) interval *= 2;
    while (interval > 800) interval /= 2;
    pet_rhy_interval = interval;
    /* The first note lands on a real beat of the song, after the count-in. */
    uint32_t first = pet_mus.beat;
    while ((int32_t)(first - now) < (PET_RHY_LEAD + 1) * interval) first += interval;
    pet_rhy_first = first;
    pet_rhy_perfect = pet_rhy_good = pet_rhy_combo = pet_rhy_best = 0;
    pet_rhy_count = -1;
    pet_rhy_last = 0;
    pet_rhy_judge_at = 0;
    memset(pet_rhy_judged, 0, sizeof pet_rhy_judged);
}

static void pet_rhy_judge(int i, int verdict, uint32_t now)
{
    pet_rhy_judged[i] = verdict;
    pet_rhy_last = verdict;
    pet_rhy_judge_at = now;
    if (verdict == 3) { pet_rhy_combo = 0; sfx_play(SFX_PET_MISS); }
    else
    {
        if (verdict == 1) pet_rhy_perfect++; else pet_rhy_good++;
        pet_rhy_combo++;
        if (pet_rhy_combo > pet_rhy_best) pet_rhy_best = pet_rhy_combo;
        sfx_play_ex(SFX_PET_CHIRP, 100 + (pet_rhy_combo > 12 ? 12 : pet_rhy_combo) * 5, 55);
    }
    pet_game_show_float(verdict == 1 ? "PERFECT!" : verdict == 2 ? "GOOD" : "MISS",
                        PET_RHY_RING + 8, 96, 450);
    if (pet_game_message)
        lv_label_set_text(pet_game_message, pet_rhy_combo >= 8 ? "ON FIRE! KEEP GOING!" :
                          pet_rhy_combo >= 4 ? "WHAT A GROOVE!" :
                          verdict == 3 ? "FIND THE BEAT AGAIN" : "TAP ON THE BEAT!");
    pet_game_set_score();
}

static void pet_rhy_tap(uint32_t now)
{
    int best = -1, gap = 0;
    for (int i = 0; i < PET_RHY_NOTES; i++)
    {
        if (pet_rhy_judged[i]) continue;
        int d = abs((int)(int32_t)(now - pet_rhy_hit(i)));
        if (best < 0 || d < gap) { best = i; gap = d; }
    }
    if (best < 0) return;
    if (gap <= PET_RHY_PERFECT) pet_rhy_judge(best, 1, now);
    else if (gap <= PET_RHY_GOOD) pet_rhy_judge(best, 2, now);
    else if (gap < pet_rhy_interval / 2)
    {
        /* Off the beat: no note spent, but the combo breaks. */
        pet_rhy_combo = 0;
        pet_game_show_float("OFF BEAT", PET_RHY_RING + 8, 96, 400);
        pet_game_set_score();
    }
}

static void pet_rhy_step(uint32_t now)
{
    int32_t to_first = (int32_t)(pet_rhy_first - now);
    int count = to_first > 0 ? (to_first + pet_rhy_interval - 1) / pet_rhy_interval : 0;
    if (count != pet_rhy_count && count > 0 && count <= 3)
    {
        char n[4];
        snprintf(n, sizeof n, "%d", count);
        pet_game_show_float(n, PET_GAME_W / 2, 40, (uint32_t)pet_rhy_interval - 40);
    }
    if (count != pet_rhy_count && count == 0 && pet_game_message)
        lv_label_set_text(pet_game_message, "TAP ON THE BEAT!");
    pet_rhy_count = count;
    bool done = true;
    for (int i = 0; i < PET_RHY_NOTES; i++)
    {
        if (!pet_rhy_judged[i] && (int32_t)(now - pet_rhy_hit(i)) > PET_RHY_GOOD)
            pet_rhy_judge(i, 3, now);
        if (!pet_rhy_judged[i]) done = false;
    }
    if (done && (int32_t)(now - pet_rhy_judge_at) > 500)
    {
        int hits = pet_rhy_perfect + pet_rhy_good;
        pet_game_finish(hits >= 11);
        if (pet_game_message && !pet.dead && !pet.manic && !pet.crisis)
        {
            char b[48];
            snprintf(b, sizeof b, hits == PET_RHY_NOTES ? "FLAWLESS! %d IN A ROW" :
                     hits >= 11 ? "WHAT A DANCE! BEST COMBO %d" : "OUT OF STEP. BEST COMBO %d",
                     pet_rhy_best);
            lv_label_set_text(pet_game_message, b);
        }
    }
}

static void pet_rhy_render(uint32_t now)
{
    unsigned t = pet_game_t;
    int32_t into = (int32_t)(now - pet_rhy_first);
    int beat = into >= 0 ? into / pet_rhy_interval : -1 - (-into - 1) / pet_rhy_interval;
    int phase = into >= 0 ? into % pet_rhy_interval : pet_rhy_interval - 1 - (-into - 1) % pet_rhy_interval;
    bool pulse = phase < 110;
    /* Dance floor: tiles that light up on the beat, and the disco ball. */
    pet_music_disco(t);
    pet_px(0, 76, PET_GAME_W, 1, 1);
    for (int k = 0; k < 8; k++)
    {
        bool lit = pulse && (k + beat) % 3 == 0;
        pet_px(k * 16 + 1, 78, 14, 8, lit ? 1 : 2);
        if (!lit) pet_px(k * 16 + 2, 79, 12, 6, 0);
    }
    /* The pet: bigger moves with a combo, a stumble just after a miss. */
    bool stumble = pet_rhy_last == 3 && now - pet_rhy_judge_at < 400;
    int lift = pulse && !stumble ? 2 + (pet_rhy_combo > 6 ? 6 : pet_rhy_combo) / 2 : 0;
    enum pet_motion motion = stumble ? PET_IDLE : pet_rhy_combo >= 4 ? PET_CLUBBING : PET_CELEBRATE;
    pet_actor_ex(48 + (stumble ? (t % 2 ? 2 : -2) : 0), 43 - lift + (stumble ? 2 : 0), motion,
                 beat < 0 ? 0 : (unsigned)beat, stumble ? (t % 4 < 2 ? 2 : -2) : 0,
                 SL_PET_GROWN, !stumble && pet_rhy_combo >= 2);
    if (pet_rhy_combo >= 8)
    {
        pet_spark(40, 40 + (int)(t % 3), 2);
        pet_spark(88, 36 + (int)((t + 1) % 3), 2);
    }
    /* Track, target ring and incoming notes. */
    pet_px(0, 104, PET_GAME_W, 1, 2);
    pet_px(0, 120, PET_GAME_W, 1, 2);
    int r = pulse ? 9 : 8;
    pet_disc(PET_RHY_RING, 112, r, 1);
    pet_disc(PET_RHY_RING, 112, r - 2, 0);
    if (pet_rhy_last && now - pet_rhy_judge_at < 250 && pet_rhy_last != 3)
        pet_game_star_burst(PET_RHY_RING, 112, 14);
    int lead = PET_RHY_LEAD * pet_rhy_interval;
    for (int i = 0; i < PET_RHY_NOTES; i++)
    {
        if (pet_rhy_judged[i]) continue;
        int32_t until = (int32_t)(pet_rhy_hit(i) - now);
        if (until > lead) continue;
        int nx = PET_RHY_RING + (int)(until * (PET_GAME_W - PET_RHY_RING) / lead);
        pet_glyph_s(pet_note_glyph, 7, nx - 5, 105, 0, 2);
    }
    /* Progress along the bottom: one pip per note. */
    for (int i = 0; i < PET_RHY_NOTES; i++)
    {
        int px = 8 + i * 7;
        int v = pet_rhy_judged[i];
        if (v == 1 || v == 2) pet_px(px, 127, 5, 5, 1);
        else if (v == 3) pet_game_x(px, 127, 5, 1);
        else { pet_px(px, 127, 5, 5, 1); pet_px(px + 1, 128, 3, 3, 0); }
    }
}

/* ---------------------------------------------------------------- loop */
static void pet_game_render(void)
{
    if (!pet_game_canvas) return;
    uint32_t now = pet_now(), elapsed = now - pet_game_started;
    pet_inks_init();
    if (pet_game_kind == PET_HEADING) pet_head_render(now);
    else
    {
        pet_target(pet_game_buf, PET_GAME_W, PET_GAME_H, 3);
        pet_px(0, 0, PET_GAME_W, PET_GAME_H, 0);
        if (pet_game_kind == PET_CATCH) pet_catch_render(now, elapsed);
        else if (pet_game_kind == PET_CUPS) pet_cups_render(now, elapsed);
        else if (pet_game_kind == PET_RHYTHM) pet_rhy_render(now);
        else pet_mem_render(now);
    }
    lv_obj_invalidate(pet_game_canvas);
}

static void pet_game_step(lv_timer_t *timer)
{
    (void)timer;
    if (!pet_game_page_obj) return;
    uint32_t now = pet_now(), elapsed = now - pet_game_started;
    pet_game_t++;
    if (!pet_game_finished)
    {
        if (pet_game_kind == PET_CATCH)
        {
            pet_bug_step(now);
            if (elapsed >= PET_CATCH_MS)
            {
                if (pet_game_message) lv_label_set_text(pet_game_message, "TIME UP!");
                pet_game_finish(pet_game_hits >= 5);
            }
        }
        else if (pet_game_kind == PET_CUPS && elapsed >= PET_CUP_READY_AT && pet_game_message &&
                 strcmp(lv_label_get_text(pet_game_message), "WHICH CUP HIDES THE PEARL?"))
        {
            if (pet_cup_chosen < 0) lv_label_set_text(pet_game_message, "WHICH CUP HIDES THE PEARL?");
        }
        else if (pet_game_kind == PET_CUPS && elapsed >= PET_CUP_SHUFFLE_AT && elapsed < PET_CUP_READY_AT &&
                 pet_game_message && strcmp(lv_label_get_text(pet_game_message), "SHUFFLING..."))
            lv_label_set_text(pet_game_message, "SHUFFLING...");
        else if (pet_game_kind == PET_HEADING) pet_head_step(now);
        else if (pet_game_kind == PET_RHYTHM) pet_rhy_step(now);
        if (pet_game_kind == PET_CUPS && elapsed >= PET_CUP_SHUFFLE_AT)
        {
            int swaps = (int)(elapsed - PET_CUP_SHUFFLE_AT) / PET_CUP_SWAP_MS + 1;
            if (swaps <= PET_CUP_SWAPS && swaps > pet_cup_swooshed)
            {
                pet_cup_swooshed = swaps;
                sfx_play_vary(SFX_PET_SWAP);
            }
        }
    }
    if (pet_game_float && (int32_t)(now - pet_game_float_until) >= 0 &&
        !lv_obj_has_flag(pet_game_float, LV_OBJ_FLAG_HIDDEN))
        lv_obj_add_flag(pet_game_float, LV_OBJ_FLAG_HIDDEN);
    pet_game_render();
}

/* Taps arrive in canvas pixels; games work in LCD pixels (128 x 136). */
static void pet_game_tap(int lx, int ly)
{
    if (pet_game_finished || !pet_game_page_obj) return;
    if (pet_game_kind == PET_CATCH)
    {
        if (pet_now() - pet_game_started < 350) return;
        pet_catch_tap(lx, ly);
    }
    else if (pet_game_kind == PET_CUPS) pet_cups_tap(lx, ly);
    else if (pet_game_kind == PET_MEMORY) pet_mem_tap(lx, ly);
    else if (pet_game_kind == PET_RHYTHM) pet_rhy_tap(pet_now());
    pet_game_render();
}

static void pet_game_press_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev || !pet_game_canvas) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    lv_area_t area;
    lv_obj_get_coords(pet_game_canvas, &area);
    (void)e;
    pet_game_tap((point.x - area.x1) / 3, (point.y - area.y1) / 3);
}

static void pet_game_deleted(lv_event_t *e)
{
    /* "Play again" replaces a game page with a fresh one; only the page that
     * is still current owns the timer and canvas pointers. */
    if (lv_event_get_current_target(e) != pet_game_page_obj) return;
    if (pet_game_timer) lv_timer_delete(pet_game_timer);
    pet_game_timer = NULL;
    pet_game_page_obj = pet_game_message = pet_game_score = NULL;
    pet_game_canvas = pet_game_float = pet_game_again = NULL;
    pet_game_pads[0] = pet_game_pads[1] = NULL;
    sl_give_back(pet_game_buf, sizeof pet_game_buf);   /* 306 KB, filled whole on every frame of a game */
    pet_refresh();
}

static void pet_game_open(int kind);
static void pet_game_again_cb(lv_event_t *e)
{
    (void)e;
    if (pet.dead) return;
    pet_game_open(pet_game_kind);
}

static void pet_game_open(int kind)
{
    pet_rng_state ^= pet_now() * 2654435761u + 0x9e3779b9u;
    pet_rand(1);
    pet_game_kind = kind;
    pet_game_hits = pet_game_misses = pet_game_input = 0;
    pet_game_target = (int)pet_rand(3);
    pet_game_started = pet_now();
    pet_game_finished = pet_game_won = false;
    pet_game_fx = PET_FX_NONE;
    pet_game_fx_at = 0;
    pet_game_t = 0;
    pet_game_float_until = 0;
    pet_motion = PET_PLAYING;
    pet_motion_until = sl_ms() + 1000;
    /* Catch Bug. */
    pet_bug_speed = 16;
    pet_bug_last = 0;
    pet_bug_respawn();
    pet_catcher_x = 48;
    /* Find the Ball: six swaps, never the same pair twice in a row. */
    pet_cup_chosen = -1;
    pet_cup_swooshed = 0;
    pet_mem_sung = -1;
    for (int i = 0; i < 3; i++) pet_cup_pos[i] = i;
    for (int s = 0; s < PET_CUP_SWAPS; s++)
    {
        int a, b;
        do { a = (int)pet_rand(3); b = (a + 1 + (int)pet_rand(2)) % 3; }
        while (s && ((a == pet_cup_swaps[s-1][0] && b == pet_cup_swaps[s-1][1]) ||
                     (a == pet_cup_swaps[s-1][1] && b == pet_cup_swaps[s-1][0])));
        pet_cup_swaps[s][0] = a; pet_cup_swaps[s][1] = b;
    }
    /* Melody Gems: five notes, never three alike in a row. */
    for (int i = 0; i < 5; i++)
    {
        uint8_t gem;
        do gem = (uint8_t)pet_rand(4);
        while (i >= 2 && gem == pet_game_pattern[i-1] && gem == pet_game_pattern[i-2]);
        pet_game_pattern[i] = gem;
    }
    pet_mem_round = 0;
    pet_mem_demo = true;
    pet_mem_phase_at = pet_now();
    pet_mem_lit = -1; pet_mem_lit_until = 0;
    /* Left or Right. */
    pet_head_round = pet_head_score = 0;
    pet_head_guess = 0;
    pet_head_dir = pet_rand(2) ? 1 : -1;
    memset(pet_head_results, 0, sizeof pet_head_results);
    /* Dance With Me. */
    pet_music_poll();
    pet_rhy_start(pet_now());

    static const char *const titles[] = { "Catch Bug", "Melody Gems", "Find the Ball", "Left or Right",
        "Dance With Me" };
    static const char *const intros[] = { "SWAT THE BUZZING BUG!", "WATCH THE GEMS LIGHT UP",
        "WATCH THE PEARL BOUNCE", "WHICH WAY WILL I LOOK?", "LISTEN FOR THE BEAT..." };
    static const char *const hints[] = {
        "TAP THE BUG FIVE TIMES IN 20 SECONDS. THREE MISSES END THE HUNT.",
        "REPEAT THE MELODY. EACH ROUND ADDS A GEM: 3, 4, THEN 5.",
        "KEEP YOUR EYE ON THE CUP HIDING THE PEARL, THEN TAP IT.",
        "GUESS WHERE YOUR PET WILL TURN. THREE OF FIVE WINS.",
        "TAP ANYWHERE ON THE SCREEN AS EACH NOTE REACHES THE RING. HIT 11 OF 16 TO WIN." };
    lv_obj_t *p, *body = page_new(titles[kind], &p);
    pet_game_page_obj = p;
    lv_obj_add_event_cb(p, pet_game_deleted, LV_EVENT_DELETE, NULL);
    pet_page_style(p);
    pet_game_message = text(body, intros[kind], &ss_semibold_21, PET_LIGHT);
    lv_obj_set_width(pet_game_message, lv_pct(100));
    lv_obj_set_style_text_align(pet_game_message, LV_TEXT_ALIGN_CENTER, 0);
    pet_game_score = text(body, "", &ss_semibold_18, PET_LCD);
    lv_obj_set_width(pet_game_score, lv_pct(100));
    lv_obj_set_style_text_align(pet_game_score, LV_TEXT_ALIGN_CENTER, 0);
    pet_game_set_score();

    lv_obj_t *bezel = box(body);
    lv_obj_set_size(bezel, PET_GAME_W * 3 + 20, PET_GAME_H * 3 + 20);
    lv_obj_set_style_margin_top(bezel, 6, 0);
    lv_obj_set_style_margin_bottom(bezel, 6, 0);
    lv_obj_set_style_margin_left(bezel, (W - 28 - (PET_GAME_W * 3 + 20)) / 2, 0);
    lv_obj_set_style_bg_color(bezel, lv_color_hex(0x10170f), 0);
    lv_obj_set_style_bg_opa(bezel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bezel, 16, 0);
    lv_obj_set_style_border_width(bezel, 3, 0);
    lv_obj_set_style_border_color(bezel, lv_color_hex(PET_MID), 0);
    pet_game_canvas = lv_canvas_create(bezel);
    lv_canvas_set_buffer(pet_game_canvas, pet_game_buf, PET_GAME_W * 3, PET_GAME_H * 3,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_center(pet_game_canvas);
    if (kind != PET_HEADING)
    {
        lv_obj_add_flag(bezel, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(bezel, pet_game_press_cb, LV_EVENT_PRESSED, NULL);
    }
    lv_obj_t *glare = box(bezel);
    lv_obj_set_size(glare, 150, 22);
    lv_obj_set_pos(glare, 20, 14);
    lv_obj_set_style_radius(glare, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(glare, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(glare, 26, 0);
    pet_game_float = text(bezel, "", &ss_bold_28, PET_DARK);
    lv_obj_set_style_bg_color(pet_game_float, lv_color_hex(PET_LIGHT), 0);
    lv_obj_set_style_bg_opa(pet_game_float, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pet_game_float, 3, 0);
    lv_obj_set_style_border_color(pet_game_float, lv_color_hex(PET_DARK), 0);
    lv_obj_set_style_pad_hor(pet_game_float, 10, 0);
    lv_obj_set_style_pad_ver(pet_game_float, 2, 0);
    lv_obj_set_style_radius(pet_game_float, SL_R_XS, 0);
    lv_obj_add_flag(pet_game_float, LV_OBJ_FLAG_HIDDEN);

    pet_game_pads[0] = pet_game_pads[1] = NULL;
    if (kind == PET_HEADING)
    {
        lv_obj_t *row = box(body);
        lv_obj_set_size(row, lv_pct(100), 70);
        lv_obj_set_style_margin_top(row, 6, 0);
        pet_game_pads[0] = pet_button(row, "< LEFT", 214, pet_heading_cb, -1);
        pet_game_pads[1] = pet_button(row, "RIGHT >", 214, pet_heading_cb, 1);
        lv_obj_align(pet_game_pads[1], LV_ALIGN_TOP_RIGHT, 0, 0);
        for (int i = 0; i < 2; i++) lv_obj_set_height(pet_game_pads[i], 64);
    }
    lv_obj_t *hint = text(body, hints[kind], &ss_regular_16, PET_LIGHT);
    lv_obj_set_width(hint, lv_pct(100));
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    pet_game_again = pet_button(body, "PLAY AGAIN", W - 28, pet_game_again_cb, 0);
    lv_obj_add_flag(pet_game_again, LV_OBJ_FLAG_HIDDEN);
    pet_game_render();
    if (pet_game_timer) lv_timer_delete(pet_game_timer);
    pet_game_timer = lv_timer_create(pet_game_step, 33, NULL);
    page_replace(p, true);
}
