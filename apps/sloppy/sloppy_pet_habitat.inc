/* Living LCD habitat: a little room with a day/night window, wall clock,
 * rug, bowl and bouncing ball. The pet roams, gazes, naps, shows thought
 * bubbles for its needs and loves being petted. Also draws the pixel meters
 * and action icons of the handheld shell. GPLv2+. */
#define PET_HAB_W 128
#define PET_HAB_H 88
#define PET_HAB_RUG_X 18
#define PET_STAT_W 150
#define PET_STAT_H 62
#define PET_ICON 12
/* Animation clock: LVGL ticks, which also drive the UI test harness. */
#define pet_now() lv_tick_get()

static uint16_t pet_hab_buf[PET_HAB_W * PET_HAB_H * 9];
/* Last frame, one value per habitat pixel (each is a 3x3 block on screen). */
static uint16_t pet_hab_prev[PET_HAB_W * PET_HAB_H];
static bool pet_hab_prev_valid;

static uint16_t pet_stat_buf[PET_STAT_W * PET_STAT_H * 9];
static uint16_t pet_icon_buf[5][PET_ICON * PET_ICON * 9];
static lv_obj_t *pet_hab_canvas, *pet_stat_canvas;
static lv_timer_t *pet_hab_timer;

enum pet_hab_mode { PET_HAB_WANDER, PET_HAB_GAZE, PET_HAB_NAP };
static enum pet_hab_mode pet_hab_mode;
static int pet_hab_x = 48 * 16, pet_hab_goal = 48, pet_hab_face = 1;
static bool pet_hab_nap_after;
static uint32_t pet_hab_until, pet_hab_pet_start, pet_hab_pet_until, pet_hab_reward_at;
static unsigned pet_hab_t;
static uint32_t pet_rng_state = 0x2545f491u;

static unsigned pet_rand(unsigned n)
{
    uint32_t x = pet_rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    pet_rng_state = x ? x : 0x2545f491u;
    return n ? pet_rng_state % n : 0;
}

/* 9x9 meter and thought-bubble icons. */
static const char *const pet_ic_drumstick[9] = {
    "....###..", "...#ooo#.", "..#oo+oo#", "..#o+ooo#", "..#oooo#.",
    "..##oo##.", ".#+#.....", "#++#.....", ".##......" };
static const char *const pet_ic_soap[9] = {
    "......##.", ".##..#++#", "#++#.#++#", "#+o#..##.", ".##.###..",
    "...#+++#.", "..#++o++#", "..#+ooo+#", "...#####." };
static const char *const pet_ic_pad[9] = {
    ".........", ".#######.", "#ooooooo#", "#o#ooo+o#", "###oo+o+#",
    "#o#ooo+o#", "#ooooooo#", ".##...##.", "........." };
static const char *const pet_ic_bolt[9] = {
    ".....###.", "....#++#.", "...#++#..", "..#++####", ".#+++++#.",
    "####++#..", "...#+#...", "..#+#....", "..##....." };
static const char *const pet_ic_flame[9] = {
    "....#....", "...#o#...", "...#oo#..", "..#o+o#..", ".#o++oo#.",
    "#o+++++o#", "#o+++oo#.", ".#ooooo#.", "..#####.." };
static const char *const pet_ic_heart[9] = {
    ".##...##.", "#+o#.#oo#", "#+oooooo#", "#ooooooo#", ".#ooooo#.",
    "..#ooo#..", "...#o#...", "....#....", "........." };
static const char *const pet_ic_pizza[9] = {
    "#########", "#+++++++#", ".#o+#+o#.", ".#+++++#.", "..#+o+#..",
    "..#+++#..", "...#+#...", "...#+#...", "....#...." };
static const char *const pet_ic_what[9] = {
    "..####...", ".#oooo#..", "#o#..#o#.", "....#o#..", "...#o#...",
    "...#o#...", "....#....", "...#o#...", "....#...." };
static const char *const pet_ic_note[9] = {
    "....##...", "....#o#..", "....#.o#.", "....#..#.", "....#....",
    "..###....", ".#ooo#...", ".#ooo#...", "..###...." };
static const char *const pet_ic_zzz[9] = {
    "#####....", "...#.....", "..#......", ".#.......", "#####.###",
    "........#", ".......#.", "......#..", "......###" };

/* 12x12 action badges: plate & fork, bathtub, gamepad, moon & pillow, aid heart. */
static const char *const pet_ic_feed[12] = {
    "#.#.........", "#.#...####..", "#.#..#oooo#.", "###.#oo++oo#",
    ".#..#o+..+o#", ".#..#o+..+o#", ".#..#oo++oo#", ".#...#oooo#.",
    ".#....####..", ".#..........", ".#..........", "............" };
static const char *const pet_ic_wash[12] = {
    "......#..##.", ".##..#+#.#+#", "#++#..#...#.", "#++#...##...",
    ".##...#++#..", "......#++#..", "############", "#++++++++++#",
    "#oooooooooo#", ".#oooooooo#.", ".##########.", "..#......#.." };
static const char *const pet_ic_games[12] = {
    "............", "............", "..########..", ".#oooooooo#.",
    "#oo#oooo+oo#", "#o###oo+o+o#", "#oo#oooo+oo#", "#oooooooooo#",
    "#ooo####ooo#", ".#o#....#o#.", ".##......##.", "............" };
static const char *const pet_ic_rest[12] = {
    "....###.....", "..##++#.....", ".#++#...#.#.", ".#+#.....#..",
    "#++#....#.#.", "#++#........", ".#++##......", "..##++###...",
    ".##########.", "#++++++++++#", "#oooooooooo#", ".##########." };
static const char *const pet_ic_help[12] = {
    ".###....###.", "#+++#..#ooo#", "#++oooooooo#", "#oooo++oooo#",
    "#oo++++++oo#", "#oo++++++oo#", ".#ooo++ooo#.", "..#oo++oo#..",
    "...#oooo#...", "....#oo#....", ".....##.....", "............" };

static void pet_icon_render(int index)
{
    static const char *const *const icons[5] = {
        pet_ic_feed, pet_ic_wash, pet_ic_games, pet_ic_rest, pet_ic_help };
    pet_inks_init();
    pet_target(pet_icon_buf[index], PET_ICON, PET_ICON, 3);
    pet_px(0, 0, PET_ICON, PET_ICON, 0);
    pet_glyph(icons[index], 12, 0, 0, 0);
}

static int pet_value(int index)
{
    int values[] = { pet.food, pet.clean, pet.joy, pet.energy, pet.stimulation };
    return index >= 0 && index < 5 ? values[index] : 0;
}

/* Heart meters for hunger and happiness, batteries for bath and energy,
 * rising flame bars for excitement. Low needs blink like a real LCD toy. */
static void pet_stats_render(void)
{
    if (!pet_stat_canvas) return;
    static const char *const *const icons[5] = {
        pet_ic_drumstick, pet_ic_soap, pet_ic_pad, pet_ic_bolt, pet_ic_flame };
    pet_inks_init();
    pet_target(pet_stat_buf, PET_STAT_W, PET_STAT_H, 3);
    pet_px(0, 0, PET_STAT_W, PET_STAT_H, 0);
    bool blink = (pet_hab_t / 4) & 1;
    for (int i = 0; i < 5; i++)
    {
        int y = 1 + i * 12, v = pet_value(i);
        bool warn = i == 4 ? v >= 55 : v < 30;
        if (!(warn && blink)) pet_glyph(icons[i], 9, 2, y + 1, 0);
        if (i < 4) pet_px(2, y + 11, PET_STAT_W - 4, 1, 2);
        if (i == 0 || i == 2)
        {
            for (int k = 0; k < 5; k++)
            {
                bool full = v >= k * 20 + 10;
                if (warn && blink && full && (k + 1) * 20 > v - 10) full = false;
                pet_glyph(pet_ic_heart, 8, 48 + k * 16, y + 2, full ? 0 : 1);
            }
        }
        else if (i == 1 || i == 3)
        {
            pet_px(47, y + 1, 76, 9, 1); pet_px(48, y + 2, 74, 7, 0);
            pet_px(123, y + 3, 3, 5, 1);
            int count = (v + 5) / 10;
            if (warn && blink && count) count--;
            for (int k = 0; k < count; k++) pet_px(49 + k * 7 + (k > 4), y + 3, 6, 5, 1);
        }
        else
        {
            int count = (v + 5) / 10;
            for (int k = 0; k < 10; k++)
            {
                int h = 2 + k * 7 / 9;
                bool on = k < count && !(warn && blink && k == count - 1);
                if (on) pet_px(48 + k * 8, y + 10 - h, 6, h, k >= 7 ? 1 : 2);
                else pet_px(48 + k * 8, y + 9, 6, 1, 2);
            }
        }
    }
    lv_obj_invalidate(pet_stat_canvas);
}

static bool pet_hab_night(void)
{
    struct tm *clock = get_time();
    int hour = clock ? clock->tm_hour : 12;
    return hour < 6 || hour >= 19;
}

static void pet_hab_rug(int cx, int cy)
{
    static const int half[] = { 12, 18, 21, 22, 21, 18, 12 };
    for (int i = 0; i < 7; i++)
    {
        pet_px(cx - half[i] - 1, cy - 3 + i, half[i] * 2 + 2, 1, 1);
        pet_px(cx - half[i] + 1, cy - 3 + i, half[i] * 2 - 2, 1, i == 3 ? 3 : 2);
    }
    for (int k = -16; k <= 16; k += 8) pet_px(cx + k, cy - 1, 2, 3, 3);
}

static void pet_hab_window(unsigned t, bool night)
{
    pet_px(7, 5, 36, 28, 1);
    pet_px(9, 7, 32, 24, night ? 1 : 0);
    if (night)
    {
        pet_disc(33, 14, 5, 3); pet_disc(35, 12, 4, 1);
        static const int stars[][2] = { {13,10}, {20,22}, {26,9}, {14,26}, {37,26}, {23,15} };
        for (int i = 0; i < 6; i++)
            if ((t / 5 + i * 3) % 7) pet_px(stars[i][0], stars[i][1], 1, 1, 3);
            else pet_spark(stars[i][0], stars[i][1], 1);
    }
    else
    {
        pet_disc(16, 13, 4, 1); pet_disc(16, 13, 3, 3);
        for (int i = 0; i < 4; i++)
        {
            static const int ray[4][2] = { {0,-6}, {6,0}, {0,6}, {-6,0} };
            if ((t / 6 + i) & 1) pet_px(16 + ray[i][0], 13 + ray[i][1], 1, 1, 1);
        }
        int cx = 9 + (int)((t / 3) % 44) - 6;
        pet_disc(cx, 22, 3, 2); pet_disc(cx + 4, 21, 4, 2); pet_disc(cx + 8, 22, 3, 2);
        pet_px(cx - 2, 23, 14, 2, 3);
        pet_px(7, 5, 2, 28, 1); pet_px(41, 5, 2, 28, 1);
    }
    pet_px(24, 7, 2, 24, 1); pet_px(9, 18, 32, 2, 1);
    pet_px(5, 33, 40, 3, 1); pet_px(6, 33, 38, 1, 2);
}

static void pet_hab_clock(void)
{
    static const int8_t dir[12][2] = {
        {0,-6}, {3,-5}, {5,-3}, {6,0}, {5,3}, {3,5},
        {0,6}, {-3,5}, {-5,3}, {-6,0}, {-5,-3}, {-3,-5} };
    struct tm *clock = get_time();
    int hour = clock ? clock->tm_hour % 12 : 3, minute = clock ? clock->tm_min : 0;
    pet_disc(111, 17, 9, 1); pet_disc(111, 17, 7, 3);
    for (int i = 0; i < 12; i += 3) pet_px(111 + dir[i][0], 17 + dir[i][1], 1, 1, 2);
    int m = minute / 5, h = (hour + (minute >= 30)) % 12;
    pet_line(111, 17, 111 + dir[m][0], 17 + dir[m][1], 1);
    pet_line(111, 17, 111 + dir[h][0] / 2, 17 + dir[h][1] / 2, 1);
    pet_px(110, 25, 3, 5, 1);
}

static void pet_hab_heart(int x, int y, int ink)
{
    pet_px(x, y, 2, 1, ink); pet_px(x + 3, y, 2, 1, ink);
    pet_px(x - 1, y + 1, 7, 2, ink); pet_px(x, y + 3, 5, 1, ink);
    pet_px(x + 1, y + 4, 3, 1, ink); pet_px(x + 2, y + 5, 1, 1, ink);
}

static void pet_hab_zzz(int x, int y, unsigned t)
{
    for (int i = 0; i < 3; i++)
    {
        int zx = x + i * 6, zy = y - i * 7 - (int)((t / 3) % 5);
        int s = 3 + i;
        pet_line(zx, zy, zx + s, zy, 1); pet_line(zx + s, zy, zx, zy + s, 1);
        pet_line(zx, zy + s, zx + s, zy + s, 1);
    }
}

static const char *const *pet_hab_need_icon(void)
{
    const char *const *needs[5];
    int count = 0;
    if (pet.food < 30) needs[count++] = pet_ic_pizza;
    if (pet.clean < 30) needs[count++] = pet_ic_soap;
    if (pet.energy < 30) needs[count++] = pet_ic_zzz;
    if (pet.joy < 30) needs[count++] = pet_ic_pad;
    if (!count && pet.food >= 70 && pet.clean >= 70 && pet.joy >= 70 &&
        pet.energy >= 50 && !pet.manic && !pet.crisis && (pet_hab_t / 40) % 3 == 0)
        needs[count++] = pet_ic_heart;
    if (!count) return NULL;
    unsigned cycle = pet_hab_t / 45;
    if ((pet_hab_t % 45) >= 34) return NULL;
    return needs[cycle % count];
}

static void pet_hab_bubble(int px, int py, const char *const *icon)
{
    int bx = px + 30, by = py - 22;
    bool left = bx > 108;
    if (left) bx = px - 20;
    if (by < 2) by = 2;
    pet_disc(left ? px + 1 : px + 29, py + 1, 1, 1);
    pet_disc(left ? px - 3 : px + 32, py - 4, 2, 1);
    pet_px(bx + 1, by, 17, 15, 1);
    pet_px(bx, by + 1, 19, 13, 1);
    pet_px(bx + 1, by + 1, 17, 13, 0);
    pet_glyph(icon, 9, bx + 5, by + 3, 0);
}

/* The ghost keeps the pet's own silhouette: pale body, dark outline eyes,
 * a halo, a wavy tail instead of feet, and a gentle flicker. */
static void pet_hab_ghost(int x, int y, unsigned t)
{
    const char *const *rows = pet_pattern(pet.species, SL_PET_GROWN);
    int bob = (int)((t / 5) % 6);
    bob = bob < 3 ? bob : 6 - bob;
    y += bob;
    bool flicker = (t % 23) == 0;
    if (!flicker)
    {
        int last = 12;
        for (int yy = 0; yy <= last; yy++) for (int xx = 0; xx < 16; xx++)
        {
            char c = rows[yy][xx];
            if (c == '.') continue;
            int sway = (yy > 8) ? (((t / 4) + yy) % 4 < 2 ? 1 : 0) : 0;
            int ink = c == '#' ? 2 : c == 'e' ? 1 : 3;
            pet_px(x + xx * 2 + sway, y + yy * 2, 2, 2, ink);
        }
        for (int k = 0; k < 6; k++)
        {
            int wave = ((int)(t / 3) + k) % 2 ? 2 : 0;
            pet_px(x + 3 + k * 5, y + 26, 4, 3 + wave, 3);
            pet_px(x + 3 + k * 5, y + 29 + wave, 4, 1, 2);
        }
        pet_px(x + 9, y - 5, 14, 1, 2); pet_px(x + 8, y - 4, 1, 1, 2);
        pet_px(x + 23, y - 4, 1, 1, 2); pet_px(x + 9, y - 3, 14, 1, 2);
    }
    if ((t / 8) % 3 == 0) pet_spark(x - 4, y + 8, 1);
    if ((t / 8) % 3 == 1) pet_spark(x + 36, y + 14, 1);
}

static void pet_hab_update(uint32_t now, enum sl_pet_stage stage)
{
    if (stage == SL_PET_EGG || pet.dead)
    { pet_hab_x = (PET_HAB_RUG_X + 2) * 16; return; }
    bool petting = (int32_t)(now - pet_hab_pet_until) < 0;
    int x = pet_hab_x / 16;
    if (petting) { pet_hab_face = 0; return; }
    if (pet.manic)
    {
        if (abs(x - pet_hab_goal) < 3) pet_hab_goal = 4 + (int)pet_rand(88);
        pet_hab_x += (pet_hab_goal > x ? 1 : -1) * 40;
        pet_hab_face = pet_hab_goal > x ? 1 : -1;
        pet_hab_mode = PET_HAB_WANDER;
        return;
    }
    /* Music: dance where it stands, look at the speaker when it stops, or
     * wander to the rug and doze off to it late at night. */
    enum pet_dance dance = pet_music_dance();
    if (dance == PET_DANCE_LULLED)
    {
        if (pet_hab_mode == PET_HAB_NAP) { pet_hab_until = now + 3000; return; }
        pet_hab_mode = PET_HAB_WANDER; pet_hab_goal = PET_HAB_RUG_X; pet_hab_nap_after = true;
    }
    else if (dance != PET_DANCE_NONE || (pet_music_wondering() && pet_hab_mode != PET_HAB_NAP))
    {
        pet_hab_mode = PET_HAB_GAZE;
        pet_hab_until = now + 800;
        pet_hab_nap_after = false;
        pet_hab_face = dance != PET_DANCE_NONE ? 0 : x < 68 ? 1 : -1;
        return;
    }
    if (pet.energy < 20 && pet_hab_mode != PET_HAB_NAP)
    {
        pet_hab_mode = PET_HAB_WANDER; pet_hab_goal = PET_HAB_RUG_X; pet_hab_nap_after = true;
    }
    if (pet_hab_mode == PET_HAB_WANDER)
    {
        if (abs(x - pet_hab_goal) <= 1)
        {
            pet_hab_x = pet_hab_goal * 16;
            if (pet_hab_nap_after)
            {
                pet_hab_mode = PET_HAB_NAP; pet_hab_nap_after = false;
                pet_hab_until = now + 5000 + pet_rand(4000);
            }
            else
            {
                pet_hab_mode = PET_HAB_GAZE;
                pet_hab_until = now + 1500 + pet_rand(2800);
            }
            pet_hab_face = 0;
        }
        else
        {
            int dir = pet_hab_goal > x ? 1 : -1;
            pet_hab_face = dir;
            pet_hab_x += dir * 12;
        }
    }
    else if ((int32_t)(now - pet_hab_until) >= 0 && pet.energy >= 20)
    {
        unsigned roll = pet_rand(100);
        bool drowsy = pet.energy < 55 || pet_hab_night();
        pet_hab_mode = PET_HAB_WANDER;
        if (drowsy && roll < 30) { pet_hab_goal = PET_HAB_RUG_X; pet_hab_nap_after = true; }
        else
        {
            int goal;
            do goal = 4 + (int)pet_rand(88); while (abs(goal - x) < 14);
            pet_hab_goal = goal;
        }
    }
}

/* The room is redrawn 12 times a second, but mostly only the pet moves:
 * send LVGL just the box that changed, not the whole 384x264 screen. */
static void pet_hab_invalidate_changed(void)
{
    int x1 = PET_HAB_W, y1 = PET_HAB_H, x2 = -1, y2 = -1;
    for (int y = 0; y < PET_HAB_H; y++)
    {
        const uint16_t *row = &pet_hab_buf[(size_t)y * 3 * PET_HAB_W * 3];
        uint16_t *prev = &pet_hab_prev[y * PET_HAB_W];
        for (int x = 0; x < PET_HAB_W; x++)
        {
            uint16_t c = row[x * 3];
            if (pet_hab_prev_valid && prev[x] == c) continue;
            prev[x] = c;
            if (x < x1) x1 = x;
            if (x > x2) x2 = x;
            if (y < y1) y1 = y;
            if (y > y2) y2 = y;
        }
    }
    pet_hab_prev_valid = true;
    if (x2 < 0) return;
    lv_area_t a;
    lv_obj_get_coords(pet_hab_canvas, &a);
    a.x2 = a.x1 + x2 * 3 + 2; a.y2 = a.y1 + y2 * 3 + 2;
    a.x1 += x1 * 3; a.y1 += y1 * 3;
    lv_obj_invalidate_area(pet_hab_canvas, &a);
}

/* Cracks on the egg (drawn at x, y, 32 pixels square), more with each tap:
 * light lines, the inside showing through the dark shell. */
static void pet_egg_crack_line(int x0, int y0, int x1, int y1)
{
    pet_line(x0, y0, x1, y1, 0);
    pet_line(x0 + 1, y0, x1 + 1, y1, 0);
}
static void pet_egg_crack(int x, int y, int cracks)
{
    if (cracks >= 1) { pet_egg_crack_line(x + 18, y + 7, x + 21, y + 11); pet_egg_crack_line(x + 21, y + 11, x + 17, y + 15); }
    if (cracks >= 2) { pet_egg_crack_line(x + 9, y + 12, x + 13, y + 16); pet_egg_crack_line(x + 13, y + 16, x + 9, y + 20); }
    if (cracks >= 3)
        for (int i = 0; i < 6; i++)             /* a zigzag right round the middle */
            pet_egg_crack_line(x + 5 + i * 4, y + 17 + (i & 1) * 3, x + 9 + i * 4, y + 17 + !(i & 1) * 3);
}

/* The shell flies apart for a moment after the egg hatches. */
static void pet_hatch_burst(int x, int y, uint32_t age)
{
    int k = (int)(age / 40);                    /* 0..35 */
    int rise = k * 3 - k * k / 8;               /* up, then down */
    for (int side = -1; side <= 1; side += 2)
    {
        int px = x + 15 + side * (8 + k * 2), py = y + 10 - rise;
        pet_px(px - 6, py, 13, 2, 1);           /* a jagged half shell */
        pet_px(px - 6, py + 2, 2, 6, 1);
        pet_px(px + 5, py + 2, 2, 6, 1);
        pet_px(px - 4, py + 2, 9, 6, 3);
        pet_px(px - 6, py + 8, 4, 2, 1); pet_px(px - 1, py + 8, 3, 2, 1); pet_px(px + 4, py + 8, 3, 2, 1);
    }
    if (k < 25) { pet_spark(x - 6 - k / 2, y + 6, 2); pet_spark(x + 36 + k / 2, y + 4, 2); pet_spark(x + 16, y - 8 - k / 2, 2); }
}

static void pet_hab_render(void)
{
    if (!pet_hab_canvas) return;
    uint32_t now = pet_now();
    unsigned t = pet_hab_t;
    enum sl_pet_stage stage = sl_pet_stage_at(&pet, pet_clock());
    bool night = pet_hab_night();
    pet_inks_init();
    pet_target(pet_hab_buf, PET_HAB_W, PET_HAB_H, 3);
    pet_px(0, 0, PET_HAB_W, PET_HAB_H, 0);
    /* Wallpaper, baseboard and floorboards. */
    for (int y = 4; y < 58; y += 9)
        for (int x = (y / 9) % 2 * 5; x < PET_HAB_W; x += 10) pet_px(x, y, 1, 1, 2);
    pet_px(0, 58, PET_HAB_W, 3, 1); pet_px(0, 61, PET_HAB_W, 1, 2);
    for (int y = 68; y < PET_HAB_H; y += 7)
    {
        pet_px(0, y, PET_HAB_W, 1, 2);
        for (int x = (y / 7) % 2 * 14; x < PET_HAB_W; x += 28) pet_px(x, y - 6, 1, 6, 2);
    }
    pet_hab_window(t, night);
    pet_hab_clock();
    /* Shelf with a potted sprout. */
    pet_px(62, 30, 26, 2, 1);
    pet_px(71, 23, 8, 7, 1); pet_px(72, 24, 6, 2, 3);
    pet_disc(72, 19, 3, 2); pet_disc(78, 18, 3, 2); pet_px(75, 17, 1, 6, 1);
    pet_music_speaker();
    pet_hab_rug(PET_HAB_RUG_X + 16, 79);
    /* Food bowl and a toy ball that bounces on its own. */
    pet_px(86, 75, 12, 5, 1); pet_px(87, 74, 10, 2, 3); pet_px(88, 80, 8, 1, 1);
    int phase = t % 26, hop = phase * (26 - phase) / 14;
    pet_px(106 - (12 - hop / 2) / 2, 81, 12 - hop / 2, 2, 2);
    pet_disc(112, 76 - hop, 4, 1); pet_disc(112, 76 - hop, 3, 3);
    pet_px(109, 76 - hop, 7, 1, 1);

    if (pet.dead)
    {
        /* Tombstone with a flower, and the pet's own ghost drifting above. */
        pet_px(44, 52, 24, 28, 1); pet_px(46, 50, 20, 4, 1);
        pet_px(46, 54, 20, 24, 2); pet_px(48, 52, 16, 2, 2);
        pet_px(55, 57, 2, 12, 1); pet_px(51, 61, 10, 2, 1);
        pet_px(38, 79, 36, 3, 1);
        pet_disc(73, 76, 2, 3); pet_px(73, 78, 1, 3, 1);
        pet_disc(39, 77, 2, 3); pet_px(39, 79, 1, 2, 1);
        int drift = (int)((t / 6) % 24);
        pet_hab_ghost(70 + (drift < 12 ? drift : 24 - drift) - 6, 8, t);
        pet_hab_x = 48 * 16;
        pet_hab_invalidate_changed();
        return;
    }

    bool petting = (int32_t)(now - pet_hab_pet_until) < 0;
    bool napping = pet_hab_mode == PET_HAB_NAP || (pet.energy < 20 && !pet.manic &&
                   pet_hab_mode != PET_HAB_WANDER);
    enum pet_motion motion = pet_motion != PET_IDLE ? pet_motion :
                             pet.manic ? PET_MANIC :
                             pet.hungover && napping ? PET_HUNGOVER :
                             napping ? PET_SLEEPING :
                             pet_hab_mode == PET_HAB_WANDER ? PET_PLAYING : PET_IDLE;
    if (petting) motion = PET_IDLE;
    enum pet_dance dance = pet_music_dance();
    bool dancing = dance != PET_DANCE_NONE && dance != PET_DANCE_LULLED && !napping &&
                   pet_motion == PET_IDLE && !petting && !pet.manic;
    if (dancing && stage != SL_PET_EGG && (dance == PET_DANCE_GROOVE || pet_mus.loved ||
                                           (dance == PET_DANCE_BOP && pet_mus.level >= 5)))
        pet_music_disco(t);
    int x = pet_hab_x / 16, y = 45;
    if (stage == SL_PET_EGG)
    {
        x = PET_HAB_RUG_X; y = 44;
        bool wobble = petting || (t % 30) < 8 || (pet_mus.playing && pet_music_phase() < 200);
        int ex = x + (wobble ? ((t & 2) ? 1 : -1) : 0);
        pet_actor_ex(ex, y, wobble ? PET_PLAYING : PET_IDLE, t, 0, SL_PET_EGG, false);
        pet_egg_crack(ex, y, pet_egg_cracks);
        if (petting && pet_egg_cracks) { pet_spark(x - 3, y + 6, 1); pet_spark(x + 18, y + 4, 1); }
    }
    else
    {
        if (motion == PET_CELEBRATE) y -= (t % 6) < 3 ? 5 : 0;
        if (motion == PET_MANIC) y -= (int)(t % 4) * 2;
        if (napping && motion == PET_SLEEPING) y = 47;
        int wobble = petting ? ((t / 2) % 2 ? 1 : -1) : 0;
        int look = petting ? 0 : pet_hab_face * 2;
        if (pet_hab_mode == PET_HAB_GAZE && !petting && (t / 25) % 4 == 3)
            look = ((t / 100) % 2) ? 1 : -1;
        if (dancing)
        {
            /* On the beat: a hop for pop, a nod for rock, a side step for
             * dance music, and a slow sway for soft songs and evenings. */
            unsigned b = pet_mus.beats;
            int phase = pet_music_phase(), lift = 2 + pet_mus.level / 3;
            int hop = phase < 120 ? lift : phase < 240 ? lift / 2 : 0;
            int sway = (int)(t / 3 % 8);
            sway = sway < 4 ? sway - 2 : 6 - sway;
            if (dance == PET_DANCE_HEADBANG)
                pet_actor_ex(x + (b % 2 ? 1 : -1), y + (phase < 140 ? 3 : 0), PET_CLUBBING,
                             b * 2 + (phase < 140), b % 2 ? 2 : -2, stage, false);
            else if (dance == PET_DANCE_GROOVE)
                pet_actor_ex(x + (b / 2 % 2 ? 3 : -3), y - hop / 2, PET_CLUBBING, b,
                             b / 2 % 2 ? 1 : -1, stage, true);
            else if (dance == PET_DANCE_SWAY)
                pet_actor_ex(x + sway, y, PET_CALMING, t / 2, 0, stage, true);
            else pet_actor_ex(x, y - hop, PET_CELEBRATE, b, 0, stage, true);
            pet_music_notes(x, y, false);
            /* A heart floats up on every beat of a song it loves. */
            int rise = phase / 45;
            if (pet_mus.loved && rise < 18)
                pet_hab_heart(x + (b % 2 ? 0 : 28), y - 2 - rise, rise < 9 ? 1 : 2);
        }
        else pet_actor_ex(x + wobble, y, motion, motion == PET_SLEEPING ? t / 4 : t / 2, look,
                          stage, petting || motion == PET_CELEBRATE);
        if (motion == PET_SLEEPING && pet_mus.playing) pet_music_notes(x, y, true);
        if (motion == PET_SLEEPING || motion == PET_HUNGOVER) pet_hab_zzz(x + 30, y - 2, t);
        if (motion == PET_MANIC)
            for (int i = 0; i < 3; i++)
            {
                int sx = x - 6 + (int)((t * 7 + i * 19) % 46), sy = 30 + (int)((t * 3 + i * 11) % 14);
                pet_line(sx, sy, sx - 3, sy + 5, 1); pet_line(sx - 3, sy + 5, sx + 2, sy + 5, 1);
                pet_line(sx + 2, sy + 5, sx - 1, sy + 10, 1);
            }
        if (motion == PET_CELEBRATE) { pet_spark(x - 6, y + 4, 3); pet_spark(x + 38, y + 2, 3); }
        if (petting)
        {
            uint32_t age = now - pet_hab_pet_start;
            for (int i = 0; i < 3; i++)
            {
                int rise = (int)(age / 40) - i * 12;
                if (rise < 0 || rise > 34) continue;
                pet_hab_heart(x + 4 + i * 11 + ((rise / 6) % 2), y - 2 - rise, i == 1 ? 1 : 2);
            }
            if ((t / 2) % 2) { pet_spark(x - 4, y + 10, 2); pet_spark(x + 36, y + 14, 2); }
        }
        else if (pet_music_wondering() && !napping && motion != PET_MANIC)
            pet_hab_bubble(x, y, pet_ic_what);
        else if (pet_music_humming() && !napping && motion != PET_MANIC)
        {
            pet_hab_bubble(x, y, pet_ic_note);
            if (t % 12 == 0) pet_mus.beats++;   /* a slow hummed tune */
            pet_music_notes(x, y, false);
        }
        else if (!dancing && motion != PET_SLEEPING && motion != PET_MANIC && motion != PET_HUNGOVER)
        {
            const char *const *icon = pet_hab_need_icon();
            if (icon) pet_hab_bubble(x, y, icon);
        }
    }
    if (pet_hatch_at && now - pet_hatch_at < 1400) pet_hatch_burst(PET_HAB_RUG_X, 44, now - pet_hatch_at);
    else pet_hatch_at = 0;
    pet_hab_invalidate_changed();
}

static void pet_hab_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!pet_page_obj || !npages || pages[npages - 1] != pet_page_obj || pet_care_locked) return;
    pet_music_poll();
    /* Nobody can see the habitat under the lock screen or a panel: skip
     * the full-canvas software render (12 times a second). */
    if (sl_power_hides_ui() || overlay_visible()) return;
    pet_hab_t++;
    enum sl_pet_stage stage = sl_pet_stage_at(&pet, pet_clock());
    pet_hab_update(pet_now(), stage);
    pet_hab_render();
    if (pet_hab_t % 4 == 0)
    {
        pet_stats_render();
        const char *mood = pet_mood_text();
        if (strcmp(lv_label_get_text(pet_mood), mood)) lv_label_set_text(pet_mood, mood);
    }
}

/* Petting: a wobble, closed-eye smile and floating hearts. Every cuddle
 * soothes; the joy reward is limited to one per minute. */
/* Tapping an egg: it wobbles and cracks a little more each time, then
 * bursts open. The new pet's needs start counting from the hatch. */
static void pet_egg_tap(void)
{
    uint32_t now = pet_now();
    if (pet_hab_pet_start && (int32_t)(now - pet_hab_pet_start) < 250) return;   /* one tap, one crack */
    pet_hab_pet_start = now;
    pet_hab_pet_until = now + 600;
    pet_egg_cracks++;
    if (pet_egg_cracks < PET_EGG_TAPS)
    {
        sfx_play_ex(SFX_PET_POKE, 90 + pet_egg_cracks * 12, 100);
        pet_refresh();
        pet_hab_render();
        return;
    }
    pet_egg_cracks = 0;
    pet_hatch_at = now;
    pet_hab_pet_until = now;               /* no egg wobble over the burst */
    sl_pet_hatch(&pet, pet_clock());
    if (!pet_save()) sl_log("pet: could not save the hatch");
    sl_log("pet: hatched");
    pet_motion = PET_CELEBRATE;
    pet_motion_until = sl_ms() + 1800;
    pet_refresh();                         /* EGG -> BABY plays the hatch sound */
    pet_hab_render();
}

static void pet_cuddle(void)
{
    uint32_t now = pet_now();
    if (pet.dead || (pet_hab_pet_start && (int32_t)(now - pet_hab_pet_start) < 600)) return;
    pet_hab_pet_start = now;
    pet_hab_pet_until = now + 1700;
    bool reward = !pet_hab_reward_at || now - pet_hab_reward_at >= 60000;
    if (reward) pet_hab_reward_at = now;
    sl_pet_advance(&pet, pet_clock());
    sl_pet_cuddle(&pet, reward);
    /* each species has its own voice */
    static const uint8_t voice[SL_PET_SPECIES_COUNT] = { 100, 112, 84, 70, 92, 124 };
    sfx_play_ex(SFX_PET_CHIRP, pet.species < SL_PET_SPECIES_COUNT ? voice[pet.species] : 100, 100);
    if (!pet_save()) sl_log("pet: could not save after cuddle");
    pet_hab_mode = PET_HAB_GAZE;
    pet_hab_until = now + 2500;
    pet_hab_face = 0;
    pet_refresh();
    pet_hab_render();
}
