/* LCD-inspired virtual pet, mini-games, and persistent care. GPLv2+. */
#include "sloppy_pet_state.h"

#define PET_FILE ROCKBOX_DIR "/sloppy/pet.dat"
#define PET_TEMP ROCKBOX_DIR "/sloppy/pet.tmp"
#define PET_BG 0x172a19
#define PET_LCD 0xb5c68a
#define PET_DARK 0x243b2b
#define PET_MID 0x617c57
#define PET_LIGHT 0xd5e0ab

enum pet_motion { PET_IDLE, PET_FEEDING, PET_WASHING, PET_PLAYING,
                  PET_SLEEPING, PET_CELEBRATE, PET_CALMING, PET_MANIC,
                  PET_CLUBBING, PET_HOSPITAL, PET_HUNGOVER, PET_FAREWELL, PET_POKE };
enum pet_game_kind { PET_CATCH, PET_MEMORY, PET_CUPS, PET_HEADING, PET_RHYTHM };
#define PET_CLUB_CHOICE 9
static struct sl_pet_state pet;
static bool pet_loaded;
static lv_obj_t *pet_page_obj, *pet_mood, *pet_name_label, *pet_stage_label;
static lv_obj_t *pet_values[5];
static lv_obj_t *pet_actions_obj, *pet_choose_obj, *pet_choose_btn, *pet_note_obj, *pet_tap_label;
static lv_obj_t *pet_mix_btn;
static void mix_page(void);
static uint16_t pet_preview_buf[5][64 * 64];
static enum pet_motion pet_motion;
static uint32_t pet_motion_until;
#define PET_EGG_TAPS 4                  /* taps that crack the egg open */
static int pet_egg_cracks;              /* taps so far on this egg */
static uint32_t pet_hatch_at;           /* pet_now() of the hatch, for the shell burst */
static uint32_t pet_care_until;
static enum sl_pet_action pet_pending_action;
static int pet_pending_food;
static bool pet_pending_club, pet_pending_hospital, pet_pending_relax, pet_pending_drugs;
static int pet_pending_sub = -1;         /* what it took, with pet_pending_drugs */

/* Every substance, every way it can go: the scene, then one of its lines.
 * The overdose lines (ONE MORE BUMP's first three) end a life. */
static const char *const pet_sub_button[SL_PET_SUBSTANCES] = {
    "SNORT DRUGS", "CHUG ENERGY DRINKS", "SMOKE A JOINT", "DROP ACID", "SIP COUGH SYRUP", "ONE MORE BUMP" };
static const char *const pet_sub_header[SL_PET_SUBSTANCES] = {
    "CRISIS", "ENERGY RUSH", "SESH", "TRIP", "SLOW MO", "LAST CALL" };
static const char *const pet_sub_caption[SL_PET_SUBSTANCES] = {
    "GETTING WILDER...", "CHUGGING...", "PUFF PUFF PASS...", "THE WALLS ARE BREATHING...",
    "SIPPING THE PURPLE...", "JUST ONE MORE..." };
static const char *const pet_sub_lines[SL_PET_SUBSTANCES][SL_PET_OUTCOMES] = {
    { "EVEN WILDER! NEEDS HELP.", "NOSEBLEED. STILL GOING. NEEDS HELP.",
      "300 WORDS A MINUTE. NEEDS HELP." },
    { "VIBRATING. IT CAN SEE SOUNDS.", "HEART GOING LIKE A DRUM SOLO.",
      "TOO MANY CANS. NEEDS HELP." },
    { "THE MUNCHIES. STARVING.", "STARED AT ITS OWN PAW FOR AN HOUR.",
      "PARANOID. HIDING UNDER THE BED." },
    { "SAW THE FACE OF GOD. IT WAS A PIZZA.", "BAD TRIP! NEEDS HELP.",
      "BECAME ONE WITH THE CARPET.", "THE WALLPAPER TALKED BACK. STILL WIRED." },
    { "MOVING IN SLOW MOTION...", "MELTED INTO THE COUCH.", "PURPLE TONGUE. NO REGRETS." },
    { "ITS HEART GAVE OUT ON THE DANCEFLOOR.", "WENT TOWARDS THE LIGHT. IT WAS A STROBE.",
      "FOUND IN THE CLUB TOILET. GONE.", "SOMEHOW STILL STANDING. NEEDS HELP.",
      "SAW ITS LIFE FLASH BY. WANTS A REPLAY." },
};
static const enum pet_motion pet_sub_motion[SL_PET_SUBSTANCES] = {
    PET_MANIC, PET_MANIC, PET_CALMING, PET_CLUBBING, PET_SLEEPING, PET_MANIC };
static const char *const pet_hospital_lines[SL_PET_HOSPITAL_OUTCOMES] = {
    "ABOUT A MONTH LATER... HEALTHY AGAIN!", "STOMACH PUMPED. SWEARS IT'S SOBER NOW.",
    "LEFT AGAINST MEDICAL ADVICE. STILL BUZZING.", "THE BILL ARRIVED. HEALTHY BUT BROKE.",
    "FELL IN LOVE WITH ITS NURSE. HEALTHY AGAIN!" };
static const char *const pet_hangover_lines[] = {
    "RESTING. HUNGOVER TOMORROW.", "WOKE UP IN THE BATHTUB. HUNGOVER.",
    "TEXTED ITS EX. HUNGOVER AND ASHAMED.", "WOKE UP IN ANOTHER CITY. HUNGOVER." };
static const char *const pet_club_deaths[] = {
    "YOUR PET DANCED ITSELF TO EXHAUSTION...", "DANCED UNTIL ITS LITTLE HEART STOPPED...",
    "CROWD-SURFED INTO THE AFTERLIFE...", "LAST SEEN DOING THE WORM. FOREVER..." };
/* Care, by outcome (sl_pet_care_roll): the plain one first. */
static const char *const pet_care_lines[5][SL_PET_CARE_OUTCOMES] = {
    [SL_PET_FEED]  = { "YUM!", "DEVOURED IT. LICKED THE PLATE.", "ONE BITE. NOT INTERESTED.",
                       "FOOD COMA. ZERO REGRETS." },
    [SL_PET_BATHE] = { "ALL CLEAN!", "LOVED EVERY BUBBLE.", "HATED IT. SULKING, DAMP.",
                       "SPLASH WAR! EVERYONE'S SOAKED." },
    [SL_PET_REST]  = { "WELL RESTED.", "SLEPT LIKE A ROCK.", "NIGHTMARE. WOKE UP SCREAMING.",
                       "SLEEPWALKED INTO THE FRIDGE." },
    [SL_PET_CALM]  = { "ALL CALM.", "REACHED ENLIGHTENMENT. BRIEFLY.", "COULDN'T SIT STILL.", "ALL CALM." },
};
static const char *const pet_away_lines[SL_PET_AWAY_EVENTS] = {
    "FOUND A SHINY COIN.", "TIDIED UP ITS CORNER. WEIRD.", "LEARNED A NEW DANCE.",
    "RAIDED THE FRIDGE.", "TOOK A VERY LONG NAP.", "WROTE A SAD POEM ABOUT YOU.",
    "KNOCKED OVER A PLANT.", "HAD A WEIRD DREAM ABOUT CHEESE." };
static const char *const pet_nature_words[SL_PET_NATURES] = {
    "IS A BIT OF A GLUTTON", "IS A NEAT FREAK", "IS A PARTY ANIMAL", "IS A SLEEPYHEAD",
    "IS MOODY, BUT LOVABLE" };
static uint32_t pet_seen_epoch;          /* when the pet page was last seen, this boot */

static unsigned pet_roll(void)
{
    static unsigned r;
    r = r * 1103515245u + 12345u + (unsigned)sl_ms();
    return r >> 8;
}

/* How it went, on its grave until a new pet: kept in a file of its own so
 * the saved pet's layout stays as it is. */
#define PET_ENDING ROCKBOX_DIR "/sloppy/pet-ending.txt"
static char pet_ending[64];
static bool pet_ending_read;
static const char *pet_ending_text(void)
{
    if (!pet_ending_read)
    {
        pet_ending_read = true;
        int fd = open(PET_ENDING, O_RDONLY);
        if (fd >= 0) { read_line(fd, pet_ending, sizeof pet_ending); close(fd); }
    }
    return pet_ending[0] ? pet_ending : NULL;
}
static void pet_ending_set(const char *s)
{
    pet_ending_read = true;
    strlcpy(pet_ending, s ? s : "", sizeof pet_ending);
    if (!s) { remove(PET_ENDING); return; }
    int fd = open(PET_ENDING, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;
    write(fd, pet_ending, strlen(pet_ending));
    close(fd);
}
static bool pet_pending_farewell, pet_pending_poke;
static lv_obj_t *pet_care_overlay, *pet_care_caption;
static enum sl_pet_species pet_pending_species;
static lv_obj_t *pet_name_input;

/* Pet pages are the handheld's dark green in every theme, so their header
 * (back arrow and title) uses the pet's light ink rather than the theme's. */
static void pet_page_style(lv_obj_t *p)
{
    lv_obj_set_style_bg_color(p, lv_color_hex(PET_BG), 0);
    lv_obj_t *hdr = lv_obj_get_child(p, 0);
    for (uint32_t i = 0; hdr && i < lv_obj_get_child_count(hdr); i++)
    {
        lv_obj_t *c = lv_obj_get_child(hdr, i);
        if (!lv_obj_check_type(c, &lv_label_class) && lv_obj_get_child_count(c)) c = lv_obj_get_child(c, 0);
        if (lv_obj_check_type(c, &lv_label_class))
            lv_obj_set_style_text_color(c, lv_color_hex(PET_LIGHT), 0);
    }
}

static uint32_t pet_clock(void)
{
    struct tm *clock = get_time();
    if (!clock) return 0;
    struct tm copy = *clock;
    time_t value = mktime(&copy);
    return value > 0 ? (uint32_t)value : 0;
}

static bool pet_save(void)
{
    mkdir(ROCKBOX_DIR "/sloppy");
    pet.checksum = sl_pet_checksum(&pet);
    int fd = open(PET_TEMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return false;
    bool okay = write(fd, &pet, sizeof pet) == sizeof pet;
    if (okay) okay = fsync(fd) == 0;
    if (close(fd) < 0) okay = false;
    if (!okay) { remove(PET_TEMP); return false; }
    if (rename(PET_TEMP, PET_FILE) < 0) { remove(PET_TEMP); return false; }
    return true;
}

static void pet_load(void)
{
    if (pet_loaded) return;
    pet_loaded = true;
    bool fresh = false;
    int fd = open(PET_FILE, O_RDONLY);
    if (fd >= 0)
    {
        struct sl_pet_state saved;
        ssize_t got = read(fd, &saved, sizeof saved);
        close(fd);
        if (got == sizeof saved && sl_pet_valid(&saved)) pet = saved;
        else if (got == sizeof(struct sl_pet_v4_state) &&
                 sl_pet_migrate_v4(&pet, (const struct sl_pet_v4_state *)&saved))
        { fresh = true; sl_log("pet: migrated PET4 save"); }
        else if (got == sizeof(struct sl_pet_v3_state) &&
                 sl_pet_migrate_v3(&pet, (const struct sl_pet_v3_state *)&saved))
        { fresh = true; sl_log("pet: migrated PET3 save"); }
        else if (got == sizeof(struct sl_pet_v2_state) &&
                 sl_pet_migrate_v2(&pet, (const struct sl_pet_v2_state *)&saved))
        { fresh = true; sl_log("pet: migrated PET2 save"); }
        else if (got == sizeof(struct sl_pet_legacy_state) &&
                 sl_pet_migrate(&pet, (const struct sl_pet_legacy_state *)&saved))
        { fresh = true; sl_log("pet: migrated old save; choose a friend to continue"); }
        else { sl_log("pet: invalid save, starting fresh"); sl_pet_init(&pet, pet_clock()); fresh = true; }
    }
    else { sl_pet_init(&pet, pet_clock()); fresh = true; }
    if (sl_pet_advance(&pet, pet_clock())) fresh = true;
    if (fresh && !pet_save()) sl_log("pet: could not save state");
}

static const char *const pet_egg[16] = {
    "................", "................", ".....######.....", "....#oooooo#....",
    "...#oooooooo#...", "..#oooooooooo#..", "..#oooooooooo#..", ".#oooooooooooo#.",
    ".#oooooooooooo#.", ".#oooooooooooo#.", "..#oooooooooo#..", "..#oooooooooo#..",
    "...#oooooooo#...", "....########....", "................", "................"
};
static const char *const pet_bun[16] = {
    "..##........##..", ".#oo#......#oo#.", ".#oo#......#oo#.", ".#ooo#....#ooo#.",
    "..#ooo####ooo#..", "..#oooooooooo#..", ".#oooooooooooo#.", ".#ooeeooooeeoo#.",
    ".#ooeeooooeeoo#.", ".#ooooommooooo#.", ".#oooooooooooo#.", "..#oooooooooo#..",
    "...#oooooooo#...", "....########....", "....##....##....", "...##......##..."
};
static const char *const pet_blob[16] = {
    "................", "................", ".....######.....", "...##oooooo##...",
    "..#oooooooooo#..", ".#oooooooooooo#.", ".#ooeeooooeeoo#.", "#oooeeooooeeooo#",
    "#oooooooooooooo#", "#oooooommoooooo#", "#oooooooooooooo#", ".#oooooooooooo#.",
    ".#oooooooooooo#.", "..############..", "...##......##...", "..##........##.."
};
static const char *const pet_bot[16] = {
    ".......##.......", ".......##.......", "......####......", "..############..",
    ".#oooooooooooo#.", ".#oooooooooooo#.", ".#ooeeooooeeoo#.", ".#ooeeooooeeoo#.",
    ".#oooooooooooo#.", ".#ooooommooooo#.", ".#oooooooooooo#.", ".#oooooooooooo#.",
    "..############..", "...#oooooooo#...", "...##......##...", "..##........##.."
};
static const char *const pet_slop[16] = {
    "................", "..###......###..", ".#ddd#....#ddd#.", "#ddddd####ddddd#",
    "#ddoooooooooodd#", ".#oooooooooooo#.", ".#ooeeooooeeoo#.", ".#ooeeooooeeoo#.",
    "..#oooooooooo#..", "..#ooo####ooo#..", "..#oo######oo#..", "...#oo####oo#...",
    "....#oooooo#....", "...##oooooo##...", "..##..####..##..", "................"
};
static const char *const pet_spaghet[16] = {
    "....s..ss..s....", "...ss.s..s.ss...", "....##s##s##....", "..##oooooooo##..",
    ".#oossoossooos#.", "#oooosoooosoooo#", "#ooeeooooeeoooo#", "#ooeeooooeeoooo#",
    ".#ooooommooooo#.", "..#oooooooooo#..", "...##oooooo##...", "..s#o#oooo#o#s..",
    ".s#o#.#oo#.#o#s.", "s#o#..#oo#..#o#s", ".ss....ss....ss.", "................"
};

static const char *const *pet_pattern(enum sl_pet_species species, enum sl_pet_stage stage)
{
    if (stage == SL_PET_EGG) return pet_egg;
    return species == SL_PET_BUN ? pet_bun : species == SL_PET_BOT ? pet_bot :
           species == SL_PET_SLOP ? pet_slop : species == SL_PET_SPAGHET ? pet_spaghet : pet_blob;
}

#include "sloppy_pet_scene.inc"
#include "sloppy_pet_music.inc"
static void pet_refresh(void);
static const char *pet_mood_text(void);
#include "sloppy_pet_habitat.inc"

static void pet_render(lv_obj_t *canvas, uint16_t *buffer, int scale,
                       enum sl_pet_species species, enum sl_pet_stage stage,
                       enum pet_motion motion, unsigned frame)
{
    const char *const *rows = pet_pattern(species, stage);
    uint16_t colors[4] = {
        lv_color_to_u16(lv_color_hex(PET_LCD)),
        lv_color_to_u16(lv_color_hex(PET_DARK)),
        lv_color_to_u16(lv_color_hex(PET_MID)),
        lv_color_to_u16(lv_color_hex(PET_LIGHT))
    };
    if (pet.manic && species == pet.species && frame & 1)
    {
        colors[0] = lv_color_to_u16(lv_color_hex(0xa6bb79));
        colors[2] = lv_color_to_u16(lv_color_hex(0xe2edb8));
    }
    int size = scale * 16;
    bool blink = motion == PET_SLEEPING || (motion == PET_IDLE && frame % 13 == 0);
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++)
    {
        char c = rows[y][x];
        if (stage == SL_PET_EGG && frame & 1 && y == 3 && x >= 7 && x <= 9) c = '#';
        if (c == 'e' && (blink || (pet.dead && species == pet.species))) c = 'o';
        if (pet.manic && species == pet.species && stage != SL_PET_EGG &&
            c == 'e' && (frame & 1)) c = '.';
        if (c == 'm' && motion == PET_FEEDING && (frame & 1)) c = '#';
        if (motion == PET_FEEDING)
        {
            int fx = 1 + (frame % 4) * 2, dx = x - fx, dy = y - 10;
            if (dx >= 0 && dx < 3 && dy >= 0 && dy < 3)
            {
                int shape = pet_pending_food % 5;
                bool filled = shape == 0 ? dy >= dx :
                              shape == 1 ? true :
                              shape == 2 ? dx != 0 || dy != 0 :
                              shape == 3 ? dx == dy || dx == 1 :
                                           dx == 0 || dx == 2 || dy == 1;
                if (filled) c = dx == 1 && dy == 1 ? 'o' : '#';
            }
        }
        if (motion == PET_WASHING && frame & 1 &&
            ((x == 2 && y == 4) || (x == 13 && y == 6) || (x == 4 && y == 12))) c = '#';
        if (motion == PET_PLAYING && x >= 12 && x <= 14 &&
            y >= (frame & 1 ? 2 : 10) && y <= (frame & 1 ? 4 : 12)) c = '#';
        if (motion == PET_SLEEPING && x >= 12 && y <= 3 &&
            (x == 13 || y == 1 || y == 3)) c = '#';
        if (motion == PET_CALMING && x >= 12 && y >= 6 && y <= 10 && x == 13)
            c = frame & 1 ? '#' : '.';
        if (motion == PET_MANIC && frame & 1 &&
            ((x == 0 && y == 5) || (x == 15 && y == 7) || (x == 1 && y == 14))) c = '#';
        if (motion == PET_CELEBRATE && frame & 1 &&
            ((x == 1 && y == 3) || (x == 14 && y == 4) || (x == 2 && y == 12))) c = '#';
        if (motion == PET_CLUBBING && ((x + y + frame) % 5 == 0) &&
            (x < 3 || x > 12)) c = '#';
        if (motion == PET_HOSPITAL && (x == 14 && y >= 2 && y <= 8 &&
            (x == 14 || y == 5))) c = '#';
        if (motion == PET_HUNGOVER && y == 13 && (x == 1 || x == 14)) c = '#';
        if (pet.manic && species == pet.species &&
            ((x == 1 && y == 1) || (x == 14 && y == 2) || (x == 2 && y == 13)))
            c = frame & 1 ? '#' : '.';
        uint16_t color = colors[c == '#' || c == 'd' || c == 'e' || c == 'm' ? 1 :
                                c == 'o' ? 2 : c == '.' ? 0 : 3];
        for (int yy = 0; yy < scale; yy++)
            for (int xx = 0; xx < scale; xx++)
                buffer[(y * scale + yy) * size + x * scale + xx] = color;
    }
    lv_obj_invalidate(canvas);
}

static lv_obj_t *pet_canvas(lv_obj_t *parent, uint16_t *buffer, int scale)
{
    lv_obj_t *canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(canvas, buffer, scale * 16, scale * 16, LV_COLOR_FORMAT_RGB565);
    return canvas;
}

static void pet_tactile(lv_obj_t *b, uint32_t face, uint32_t edge)
{
    lv_obj_set_style_bg_color(b, lv_color_hex(face), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(PET_DARK), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, SL_R_MD, 0);
    lv_obj_set_style_border_width(b, 3, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(PET_DARK), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_shadow_offset_y(b, 5, 0);
    lv_obj_set_style_shadow_spread(b, 0, 0);
    lv_obj_set_style_shadow_color(b, lv_color_hex(edge), 0);
    lv_obj_set_style_shadow_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_offset_y(b, 1, LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(b, 4, LV_STATE_PRESSED);
    lv_obj_set_style_opa(b, LV_OPA_50, LV_STATE_DISABLED);
}

static lv_obj_t *pet_button(lv_obj_t *parent, const char *label, int width,
                            lv_event_cb_t cb, intptr_t data)
{
    lv_obj_t *b = box(parent);
    lv_obj_set_size(b, width, 54);
    lv_obj_set_style_margin_bottom(b, 6, 0);
    pet_tactile(b, PET_MID, 0x0d170f);
    lv_obj_center(text(b, label, &ss_semibold_21, PET_LIGHT));
    on_click(b, cb, (void *)data);
    return b;
}

/* One mood, said two ways: on the pet's LCD and in the notification. */
enum { PM_DEAD, PM_CRISIS, PM_MANIC, PM_HUNGOVER, PM_WIRED, PM_HUNGRY, PM_DIRTY,
       PM_BORED, PM_SLEEPY, PM_CARE, PM_HAPPY };
static const char *const pet_mood_lcd[] = { "GOODBYE, LITTLE FRIEND", "TOO WIRED! NEEDS HELP",
    "MANIC! CHOOSE A PATH", "HUNGOVER TODAY", "GETTING WIRED", "HUNGRY", "NEEDS A WASH",
    "WANTS A GAME", "SLEEPY", "NEEDS CARE", "HAPPY" };
static const char *const pet_mood_words[] = { "is gone", "is overwhelmed", "is bouncing off the walls",
    "is a bit hungover", "is getting wired", "is hungry", "needs a bath", "is bored",
    "is sleepy", "needs some care", "is happy" };
static int pet_mood_of(const struct sl_pet_state *s)
{
    int values[] = { s->food, s->clean, s->joy, s->energy };
    int lowest = values[0], index = 0;
    for (int i = 1; i < 4; i++) if (values[i] < lowest) { lowest = values[i]; index = i; }
    return s->dead ? PM_DEAD : s->crisis ? PM_CRISIS : s->manic ? PM_MANIC :
           s->hungover && pet_clock() >= s->hungover_until - 24 * 3600u ? PM_HUNGOVER :
           s->stimulation >= 55 ? PM_WIRED :
           lowest < 30 ? PM_HUNGRY + index : lowest < 60 ? PM_CARE : PM_HAPPY;
}

/* Music talk replaces the everyday moods, never an urgent need. */
static const char *pet_mood_text(void)
{
    if (sl_pet_is_egg(&pet))
    {
        static const char *const egg[PET_EGG_TAPS] = {
            "SOMETHING STIRS INSIDE", "CRACK!", "CRACK! CRACK!", "IT'S COMING OUT!" };
        return egg[pet_egg_cracks < PET_EGG_TAPS ? pet_egg_cracks : PET_EGG_TAPS - 1];
    }
    int mood = pet_mood_of(&pet);
    const char *music = mood >= PM_BORED ?
                        pet_music_caption(sl_pet_stage_at(&pet, pet_clock())) : NULL;
    return music ? music : pet_mood_lcd[mood];
}

/* Who it is, on its screen when nothing else is to say */
static const char *pet_nature_text(void)
{
    static char b[64];
    char name[SL_PET_NAME_SIZE];
    strlcpy(name, pet.name[0] ? pet.name : "YOUR FRIEND", sizeof name);
    for (char *c = name; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
    snprintf(b, sizeof b, "%s %s", name, pet_nature_words[sl_pet_nature(&pet)]);
    return b;
}

static void pet_refresh(void)
{
    if (!pet_page_obj) return;
    for (int i = 0; i < 5; i++)
    {
        char b[12];
        snprintf(b, sizeof b, "%d%%", pet_value(i));
        if (pet_values[i]) lv_label_set_text(pet_values[i], b);
    }
    lv_label_set_text(pet_mood, pet_mood_text());
    lv_label_set_text(pet_name_label, pet.name);
    enum sl_pet_stage stage = sl_pet_stage_at(&pet, pet_clock());
    if (stage != SL_PET_EGG && !strcmp(lv_label_get_text(pet_stage_label), "EGG"))
        sfx_play(SFX_PET_HATCH);
    lv_label_set_text(pet_stage_label, stage == SL_PET_EGG ? "EGG" :
                      stage == SL_PET_BABY ? "BABY" : "GROWN");
    pet_stats_render();
    /* Gone, or still in its egg: no needs to meet, so no needs panel. */
    bool egg = sl_pet_is_egg(&pet);
    if (pet_stat_canvas)
    {
        lv_obj_t *panel = lv_obj_get_parent(pet_stat_canvas);
        if (pet.dead || egg) lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(panel, LV_OBJ_FLAG_HIDDEN);
    }
    /* Alive: five care buttons. Gone: a ghost, a stick, and a fresh start.
     * An egg: only the egg, until it is tapped open. */
    if (pet_actions_obj && pet_choose_obj)
    {
        if (egg)
        {
            lv_obj_add_flag(pet_actions_obj, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(pet_choose_obj, LV_OBJ_FLAG_HIDDEN);
        }
        else if (pet.dead)
        {
            lv_obj_add_flag(pet_actions_obj, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(pet_choose_obj, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_remove_flag(pet_actions_obj, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(pet_choose_obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (pet_mix_btn) lv_obj_update_flag(pet_mix_btn, LV_OBJ_FLAG_HIDDEN, egg || pet.dead);
    const char *memory = pet_music_memory();
    if (pet_note_obj)
        lv_label_set_text(pet_note_obj, egg ? "YOUR NEW FRIEND IS WAITING TO MEET YOU" :
                          pet.dead ? (pet_ending_text() ? pet_ending_text() : "YOUR FRIEND'S SPIRIT STILL VISITS") :
                          memory ? memory : pet_nature_text());
    if (pet_tap_label)
        lv_label_set_text(pet_tap_label, egg ? "TAP THE EGG TO HATCH IT" :
                          pet.dead ? "REST IN PEACE" : "TAP TO CUDDLE  -  HOLD FOR MORE");
}

/* Forget a care action that never ran. A leftover flag would hijack the
 * next care scene (a pending farewell turned the next Feed into a death). */
static void pet_clear_pending(void)
{
    pet_pending_club = pet_pending_hospital = pet_pending_relax = pet_pending_drugs = false;
    pet_pending_farewell = pet_pending_poke = false;
    pet_pending_sub = -1;
}

static void pet_page_deleted(lv_event_t *e)
{
    if (lv_event_get_current_target(e) != pet_page_obj) return;
    pet_scene_close();
    if (pet_hab_timer) lv_timer_delete(pet_hab_timer);
    pet_hab_timer = NULL;
    pet_page_obj = pet_mood = pet_name_label = pet_stage_label = NULL;
    pet_actions_obj = pet_choose_obj = pet_choose_btn = pet_note_obj = pet_tap_label = NULL;
    pet_mix_btn = NULL;
    pet_hab_canvas = pet_stat_canvas = NULL;
    pet_care_locked = false;
    pet_clear_pending();
    for (int i = 0; i < 5; i++) pet_values[i] = NULL;
    /* 0.6 MB of pictures, each filled whole when drawn: back to the system
     * until the pet is opened again (they stayed the player's for good) */
    sl_give_back(pet_hab_buf, sizeof pet_hab_buf);
    sl_give_back(pet_stat_buf, sizeof pet_stat_buf);
    sl_give_back(pet_scene_buf, sizeof pet_scene_buf);
}

static void pet_game_menu(void);
static void pet_select_page(bool replace);
static void pet_food_menu(void);
static void pet_mania_menu(void);

static void pet_begin_care(enum sl_pet_action action, const char *caption,
                           enum pet_motion motion, uint32_t duration)
{
    if (pet_care_locked) return;           /* the running scene owns the flags */
    if (sl_pet_is_egg(&pet)) { pet_clear_pending(); toast("Hatch the egg first!"); return; }
    if ((pet.dead && !pet_pending_poke) || !pet_page_obj) { pet_clear_pending(); return; }
    if (sl_pet_advance(&pet, pet_clock())) pet_save();
    if (pet.dead && !pet_pending_poke)
    { pet_clear_pending(); pet_refresh(); toast("Your pet has passed away."); return; }
    pet_pending_action = action;
    pet_care_locked = true;
    pet_care_started = sl_ms();
    pet_care_duration = duration;
    pet_care_until = pet_care_started + duration;
    pet_motion = motion;
    pet_motion_until = pet_care_until;

    pet_care_overlay = box(ui_screen());
    lv_obj_set_size(pet_care_overlay, W, H);
    lv_obj_set_style_bg_color(pet_care_overlay, lv_color_hex(PET_BG), 0);
    lv_obj_set_style_bg_opa(pet_care_overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(pet_care_overlay, LV_OBJ_FLAG_CLICKABLE);

    const char *header = action == SL_PET_FEED ? "MEAL TIME" :
                         action == SL_PET_BATHE ? "BATH TIME" :
                         action == SL_PET_REST && !pet_pending_farewell ? "BEDTIME" :
                         pet_pending_hospital ? "HOSPITAL CLINIC" :
                         pet_pending_club ? "DISCO NIGHT" :
                         pet_pending_farewell ? "FAREWELL" :
                         pet_pending_poke ? "POKE" :
                         pet_pending_drugs && pet_pending_sub >= 0 ? pet_sub_header[pet_pending_sub] :
                         pet_pending_drugs ? "CRISIS" : "MEDITATION";
    lv_obj_t *hdr = text(pet_care_overlay, header, &ss_semibold_23, PET_LIGHT);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 48);

    lv_obj_t *lcd_bezel = box(pet_care_overlay);
    lv_obj_set_size(lcd_bezel, 400, 352);
    lv_obj_align(lcd_bezel, LV_ALIGN_CENTER, 0, -30);
    lv_obj_set_style_bg_color(lcd_bezel, lv_color_hex(PET_LCD), 0);
    lv_obj_set_style_bg_opa(lcd_bezel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(lcd_bezel, 4, 0);
    lv_obj_set_style_border_color(lcd_bezel, lv_color_hex(PET_MID), 0);
    lv_obj_set_style_radius(lcd_bezel, 6, 0);

    pet_care_canvas = lv_canvas_create(lcd_bezel);
    lv_canvas_set_buffer(pet_care_canvas, pet_scene_buf, PET_SCENE_W * PET_SCENE_SCALE,
                         PET_SCENE_H * PET_SCENE_SCALE, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(pet_care_canvas);

    pet_care_caption = text(pet_care_overlay, caption, &ss_semibold_26, PET_LIGHT);
    lv_obj_align(pet_care_caption, LV_ALIGN_CENTER, 0, 185);

    pet_care_progress = lv_bar_create(pet_care_overlay);
    lv_obj_set_size(pet_care_progress, 384, 14);
    lv_obj_align(pet_care_progress, LV_ALIGN_CENTER, 0, 225);
    lv_obj_set_style_radius(pet_care_progress, 0, 0);
    lv_obj_set_style_radius(pet_care_progress, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(pet_care_progress, lv_color_hex(0x8aa372), 0);
    lv_obj_set_style_bg_color(pet_care_progress, lv_color_hex(PET_LIGHT), LV_PART_INDICATOR);
    lv_bar_set_range(pet_care_progress, 0, 1000);
    lv_bar_set_value(pet_care_progress, 0, LV_ANIM_OFF);

    pet_scene_render(0);
    pet_scene_timer = lv_timer_create(pet_scene_tick, 33, NULL);
    pet_care_beat = 0;
    if (motion == PET_SLEEPING) sfx_play(SFX_PET_LULLABY);
    else if (motion == PET_CALMING) sfx_play(SFX_PET_CALM);
    else if (motion == PET_MANIC) sfx_play(SFX_PET_WILD);
    else if (motion == PET_FAREWELL) sfx_play(SFX_PET_FAREWELL);
    else pet_care_sound(0);
    lv_obj_move_foreground(pet_care_overlay);
}

static void pet_food_cb(lv_event_t *e)
{
    static const char *const foods[] = { "Pizza", "Cake", "Apple", "Carrot", "Sushi",
        "Pasta", "Burger", "Cookie", "Banana", "Taco", "Cheese", "Soup",
        "Dumpling", "Ice cream", "Noodles" };
    int food = (int)(intptr_t)lv_event_get_user_data(e);
    if (food < 0 || food >= 15) return;
    page_pop();
    pet_pending_food = food;
    char caption[48];
    snprintf(caption, sizeof caption, "MUNCHING %s...", foods[food]);
    pet_begin_care(SL_PET_FEED, caption, PET_FEEDING, 2200);
}

/* Every food on its own little LCD, drawn with the same pixel sprites the
 * pet eats in the meal scene. */
#define PET_FOOD_ICON 30
static uint16_t pet_food_icons[15][PET_FOOD_ICON * PET_FOOD_ICON * 4];
/* drawn whole each time the menu opens: 108 KB back once it closes */
static void pet_food_menu_deleted(lv_event_t *e)
{
    (void)e;
    sl_give_back(pet_food_icons, sizeof pet_food_icons);
}
static void pet_food_menu(void)
{
    uint16_t (*icons)[PET_FOOD_ICON * PET_FOOD_ICON * 4] = pet_food_icons;
    lv_obj_t *p, *body = page_new("Choose Food", &p);
    lv_obj_add_event_cb(p, pet_food_menu_deleted, LV_EVENT_DELETE, NULL);
    pet_page_style(p);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(body, 10, 0);
    lv_obj_set_style_pad_row(body, 12, 0);
    static const char *const foods[] = { "PIZZA", "CAKE", "APPLE", "CARROT", "SUSHI",
        "PASTA", "BURGER", "COOKIE", "BANANA", "TACO", "CHEESE", "SOUP",
        "DUMPLING", "ICE CREAM", "NOODLES" };
    int cell = (W - 28 - 20) / 3;
    pet_inks_init();
    for (int i = 0; i < 15; i++)
    {
        lv_obj_t *b = box(body);
        lv_obj_set_size(b, cell, 132);
        pet_tactile(b, PET_MID, 0x0d170f);
        on_click(b, pet_food_cb, (void *)(intptr_t)i);
        lv_obj_t *lcd = box(b);
        lv_obj_set_size(lcd, 76, 76);
        lv_obj_align(lcd, LV_ALIGN_TOP_MID, 0, 10);
        lv_obj_set_style_bg_color(lcd, lv_color_hex(PET_LCD), 0);
        lv_obj_set_style_bg_opa(lcd, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(lcd, SL_R_SM, 0);
        lv_obj_set_style_border_width(lcd, 2, 0);
        lv_obj_set_style_border_color(lcd, lv_color_hex(PET_DARK), 0);
        pet_target(icons[i], PET_FOOD_ICON, PET_FOOD_ICON, 2);
        pet_px(0, 0, PET_FOOD_ICON, PET_FOOD_ICON, 0);
        pet_food_draw(i, 3, 4, 0);
        lv_obj_t *icon = lv_canvas_create(lcd);
        lv_canvas_set_buffer(icon, icons[i], PET_FOOD_ICON * 2, PET_FOOD_ICON * 2, LV_COLOR_FORMAT_RGB565);
        lv_obj_center(icon);
        lv_obj_t *name = text(b, foods[i], &ss_semibold_18, PET_LIGHT);
        lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -12);
    }
    page_push(p);
}

/* choice: 0 relax, 2 hospital, 10 + a substance */
static void pet_mania_choice_cb(lv_event_t *e)
{
    int choice = (int)(intptr_t)lv_event_get_user_data(e);
    page_pop();
    int sub = choice >= 10 && choice < 10 + SL_PET_SUBSTANCES ? choice - 10 : -1;
    pet_pending_drugs = sub >= 0;
    pet_pending_sub = sub;
    pet_pending_hospital = choice == 2;
    pet_pending_relax = choice == 0;
    if (sub >= 0)
    {
        pet_begin_care(SL_PET_CALM, pet_sub_caption[sub], pet_sub_motion[sub], sub == SL_PET_BUMP ? 2800 : 2200);
        return;
    }
    pet_begin_care(SL_PET_CALM, choice == 2 ? "HOSPITAL VISIT..." : "TAKING DEEP BREATHS...",
        choice == 2 ? PET_HOSPITAL : PET_CALMING, choice == 2 ? 3500 : 2200);
}

static void pet_mania_menu(void)
{
    lv_obj_t *p, *body = page_new(pet.crisis ? "Needs Help" : "Manic", &p);
    pet_page_style(p);
    text(body, pet.crisis ? "YOUR PET IS OVERWHELMED" : "YOUR PET IS TOO EXCITED",
         &ss_semibold_23, PET_LIGHT);
    pet_button(body, "RELAX", W - 28, pet_mania_choice_cb, 0);
    if (pet.crisis)
    {
        pet_button(body, "HOSPITAL VISIT", W - 28, pet_mania_choice_cb, 2);
        pet_button(body, pet_sub_button[SL_PET_BUMP], W - 28, pet_mania_choice_cb, 10 + SL_PET_BUMP);
    }
    else
        for (int i = 0; i < SL_PET_BUMP; i++)
            pet_button(body, pet_sub_button[i], W - 28, pet_mania_choice_cb, 10 + i);
    page_push(p);
}

static void pet_action_cb(lv_event_t *e)
{
    if (pet_care_locked) return;
    if (pet.dead) { toast("Choose a new pet to begin again."); return; }
    enum sl_pet_action action = (enum sl_pet_action)(intptr_t)lv_event_get_user_data(e);
    sfx_play(SFX_PET_SELECT);
    if (action == SL_PET_FEED) { pet_food_menu(); return; }
    if (pet.manic && action == SL_PET_CALM) { pet_mania_menu(); return; }
    if (action == SL_PET_PLAY)
    {
        pet_game_menu(); return;
    }
    pet_begin_care(action, action == SL_PET_BATHE ? "SPLISH SPLASH..." :
        action == SL_PET_REST ? "CATCHING SOME ZZZ..." : "CALMING DOWN...",
        action == SL_PET_BATHE ? PET_WASHING :
        action == SL_PET_REST ? PET_SLEEPING : PET_CALMING, 2200);
}

static void pet_screen_cb(lv_event_t *e)
{
    (void)e;
    if (pet_care_locked) return;
    if (pet.dead) { toast("Choose a new pet to begin again."); return; }
    if (sl_pet_is_egg(&pet)) { pet_egg_tap(); return; }
    if (pet.manic || pet.crisis) { pet_mania_menu(); return; }
    pet_cuddle();
}

static void pet_reselect_cb(lv_event_t *e)
{ (void)e; pet_select_page(true); }

static void pet_poke_cb(lv_event_t *e)
{
    (void)e;
    if (!pet.dead || pet_care_locked) return;
    pet_pending_poke = true;
    pet_begin_care(SL_PET_PLAY, "POKE...", PET_POKE, 4200);
    if (!pet_care_locked) pet_pending_poke = false;
}

static void pet_end_confirm_cb(lv_event_t *e)
{
    (void)e;
    page_pop();
    /* a quiet goodbye first: one last cuddle, a peaceful sleep, and the
     * pet's ghost rising over its little stone; then choose a new friend */
    pet_pending_farewell = true;
    pet_begin_care(SL_PET_REST, "ONE LAST CUDDLE", PET_FAREWELL, 6500);
    if (!pet_care_locked)                  /* could not start: end it plainly */
    {
        pet_pending_farewell = false;
        sl_pet_end_life(&pet);
        pet_save();
        pet_refresh();
    }
}

static void pet_end_cancel_cb(lv_event_t *e)
{ (void)e; page_pop(); }

static void pet_long_press_cb(lv_event_t *e)
{
    (void)e;
    if (pet_care_locked || pet.dead || sl_pet_is_egg(&pet)) return;
    lv_obj_t *p, *body = page_new("End Pet Life?", &p);
    pet_page_style(p);
    lv_obj_t *note = text(body,
        "JISATSU ENDS THIS PET'S LIFE. YOU CAN CHOOSE A NEW FRIEND AFTERWARDS.",
        &ss_semibold_21, PET_LIGHT);
    lv_obj_set_width(note, lv_pct(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    pet_button(body, "CANCEL", W - 28, pet_end_cancel_cb, 0);
    pet_button(body, "END LIFE & CHOOSE NEW PET", W - 28, pet_end_confirm_cb, 0);
    page_push(p);
}

static uint32_t pet_shell_color(void)
{
    switch (pet.species) {
    case SL_PET_SLOP: return 0xe0873f;
    case SL_PET_SPAGHET: return 0xd65a48;
    case SL_PET_BUN: return 0xe58aae;
    case SL_PET_BOT: return 0x8699b8;
    default: return 0x3fa596;
    }
}

static void pet_screw(lv_obj_t *parent, int x, int y)
{
    lv_obj_t *s = box(parent);
    lv_obj_set_size(s, 16, 16);
    lv_obj_set_pos(s, x, y);
    lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s, lv_color_hex(0xd9dde2), 0);
    lv_obj_set_style_bg_grad_color(s, lv_color_hex(0x7f868f), 0);
    lv_obj_set_style_bg_grad_dir(s, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s, 2, 0);
    lv_obj_set_style_border_color(s, lv_color_hex(0x4b5058), 0);
    lv_obj_t *slot = box(s);
    lv_obj_set_size(slot, 10, 2);
    lv_obj_center(slot);
    lv_obj_set_style_bg_color(slot, lv_color_hex(0x3a3f46), 0);
    lv_obj_set_style_bg_opa(slot, LV_OPA_COVER, 0);
}

static void pet_mix_cb(lv_event_t *e) { (void)e; if (!pet_care_locked) mix_page(); }

static void pet_page_core(bool replace)
{
    lv_obj_t *p, *body = page_new("Pocket Pet", &p);
    pet_page_obj = p;
    lv_obj_add_event_cb(p, pet_page_deleted, LV_EVENT_DELETE, NULL);
    pet_page_style(p);
    uint32_t shell_color = pet_shell_color();
    lv_color_t shell = lv_color_hex(shell_color);

    /* The handheld: a rounded shell with screws, a lettered brand, and a
     * dark bezel around the living LCD room. */
    lv_obj_t *device = box(body);
    lv_obj_set_size(device, lv_pct(100), 462);
    lv_obj_set_style_margin_bottom(device, 8, 0);
    lv_obj_set_style_bg_color(device, shell, 0);
    lv_obj_set_style_bg_grad_color(device, lv_color_darken(shell, 40), 0);
    lv_obj_set_style_bg_grad_dir(device, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(device, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(device, 56, 0);
    lv_obj_set_style_border_width(device, 4, 0);
    lv_obj_set_style_border_color(device, lv_color_darken(shell, 110), 0);
    lv_obj_t *rim = box(device);
    lv_obj_set_size(rim, 436, 446);
    lv_obj_align(rim, LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_set_style_radius(rim, 52, 0);
    lv_obj_set_style_border_width(rim, 3, 0);
    lv_obj_set_style_border_side(rim, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(rim, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_opa(rim, 70, 0);
    pet_screw(device, 30, 22); pet_screw(device, 398, 22);
    pet_screw(device, 30, 420); pet_screw(device, 398, 420);
    lv_obj_t *brand = text(device, "SLOPPY GOTCHI", &ss_semibold_18, 0x2a1d17);
    lv_obj_align(brand, LV_ALIGN_TOP_MID, 0, 14);
    lv_obj_set_style_text_letter_space(brand, 3, 0);

    lv_obj_t *bezel = box(device);
    lv_obj_set_size(bezel, PET_HAB_W * 3 + 24, PET_HAB_H * 3 + 82);
    lv_obj_align(bezel, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_set_style_bg_color(bezel, lv_color_hex(0x151d16), 0);
    lv_obj_set_style_bg_opa(bezel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bezel, 22, 0);
    lv_obj_set_style_border_width(bezel, 3, 0);
    lv_obj_set_style_border_color(bezel, lv_color_darken(shell, 150), 0);
    lv_obj_set_style_shadow_width(bezel, 18, 0);
    lv_obj_set_style_shadow_color(bezel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(bezel, 90, 0);
    lv_obj_set_style_shadow_offset_y(bezel, 4, 0);
    on_click(bezel, pet_screen_cb, NULL);
    lv_obj_add_event_cb(bezel, pet_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);
    pet_name_label = text(bezel, "", &ss_semibold_21, PET_LCD);
    lv_obj_set_pos(pet_name_label, 14, 8);
    pet_stage_label = text(bezel, "", &ss_semibold_18, PET_MID);
    lv_obj_align(pet_stage_label, LV_ALIGN_TOP_RIGHT, -14, 10);
    pet_hab_canvas = lv_canvas_create(bezel);
    pet_hab_prev_valid = false;             /* a new canvas: draw it all once */
    lv_canvas_set_buffer(pet_hab_canvas, pet_hab_buf, PET_HAB_W * 3, PET_HAB_H * 3,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(pet_hab_canvas, 9, 38);
    lv_obj_t *glare = box(bezel);
    lv_obj_set_size(glare, 170, 20);
    lv_obj_set_pos(glare, 24, 46);
    lv_obj_set_style_radius(glare, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(glare, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(glare, 30, 0);
    lv_obj_t *glint = box(bezel);
    lv_obj_set_size(glint, 34, 20);
    lv_obj_set_pos(glint, 202, 46);
    lv_obj_set_style_radius(glint, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(glint, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(glint, 22, 0);
    pet_mood = text(bezel, "", &ss_semibold_21, PET_LCD);
    lv_obj_align(pet_mood, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_t *speaker = box(device);
    lv_obj_set_size(speaker, 60, 10);
    lv_obj_align(speaker, LV_ALIGN_TOP_MID, 0, 430);
    for (int i = 0; i < 4; i++)
    {
        lv_obj_t *slot = box(speaker);
        lv_obj_set_size(slot, 9, 9);
        lv_obj_set_pos(slot, i * 17, 0);
        lv_obj_set_style_radius(slot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(slot, lv_color_darken(shell, 120), 0);
        lv_obj_set_style_bg_opa(slot, LV_OPA_COVER, 0);
    }
    pet_tap_label = text(device, "TAP TO CUDDLE  -  HOLD FOR MORE", &ss_regular_16, 0x140d0a);   /* 4.5:1 on the teal device */
    lv_obj_align(pet_tap_label, LV_ALIGN_TOP_MID, 0, 398);

    /* Meters: heart, battery and flame bars with pixel icons. */
    lv_obj_t *stats = box(body);
    lv_obj_set_size(stats, lv_pct(100), PET_STAT_H * 3 + 20);
    lv_obj_set_style_margin_bottom(stats, 8, 0);
    lv_obj_set_style_bg_color(stats, lv_color_hex(PET_LCD), 0);
    lv_obj_set_style_bg_opa(stats, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(stats, 18, 0);
    lv_obj_set_style_border_width(stats, 5, 0);
    lv_obj_set_style_border_color(stats, lv_color_hex(0x151d16), 0);
    pet_stat_canvas = lv_canvas_create(stats);
    lv_canvas_set_buffer(pet_stat_canvas, pet_stat_buf, PET_STAT_W * 3, PET_STAT_H * 3,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(pet_stat_canvas, 1, 5);
    static const char *names[] = { "FOOD", "BATH", "FUN", "ENERGY", "EXCITE" };
    for (int i = 0; i < 5; i++)
    {
        lv_obj_t *name = text(stats, names[i], &ss_semibold_18, PET_DARK);
        lv_obj_set_pos(name, 40, 13 + i * 36);
        pet_values[i] = text(stats, "", &ss_semibold_18, PET_DARK);
        lv_obj_set_width(pet_values[i], 58);
        lv_obj_set_style_text_align(pet_values[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(pet_values[i], 380, 13 + i * 36);
    }

    /* Tactile action buttons with pixel badges. */
    lv_obj_t *actions = pet_actions_obj = box(body);
    lv_obj_set_size(actions, lv_pct(100), 3 * 94);
    static const char *labels[] = { "FEED", "WASH", "GAMES", "REST", "CALM / HELP" };
    for (int i = 0; i < 5; i++)
    {
        lv_obj_t *button = box(actions);
        lv_obj_set_size(button, i == 4 ? 452 : 220, 82);
        lv_obj_set_pos(button, i == 4 ? 0 : (i % 2) * 232, (i / 2) * 94);
        pet_tactile(button, PET_MID, 0x0d170f);
        on_click(button, pet_action_cb, (void *)(intptr_t)i);
        lv_obj_t *badge = box(button);
        lv_obj_set_size(badge, 52, 52);
        lv_obj_align(badge, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_set_style_bg_color(badge, lv_color_hex(PET_LCD), 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(badge, 12, 0);
        lv_obj_set_style_border_width(badge, 2, 0);
        lv_obj_set_style_border_color(badge, lv_color_hex(PET_DARK), 0);
        pet_icon_render(i);
        lv_obj_t *icon = lv_canvas_create(badge);
        lv_canvas_set_buffer(icon, pet_icon_buf[i], PET_ICON * 3, PET_ICON * 3, LV_COLOR_FORMAT_RGB565);
        lv_obj_center(icon);
        lv_obj_t *label = text(button, labels[i], &ss_semibold_23, PET_LIGHT);
        lv_obj_align(label, i == 4 ? LV_ALIGN_CENTER : LV_ALIGN_LEFT_MID, i == 4 ? 20 : 76, 0);
    }
    lv_obj_t *note = pet_note_obj = text(body, "CARE, PLAY AND WATCH YOUR FRIEND GROW", &ss_regular_16, PET_LIGHT);
    lv_obj_set_width(note, lv_pct(100));
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_ver(note, 8, 0);
    pet_mix_btn = pet_button(body, "MAKE ME A MIXTAPE", W - 28, pet_mix_cb, 0);
    /* Gone: poke it with a stick, or choose a new friend. */
    pet_choose_obj = box(body);
    lv_obj_set_size(pet_choose_obj, W - 28, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(pet_choose_obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(pet_choose_obj, 10, 0);
    lv_obj_t *poke = pet_button(pet_choose_obj, "POKE IT WITH A STICK", W - 28, pet_poke_cb, 0);
    lv_obj_set_height(poke, 84);
    pet_choose_btn = pet_button(pet_choose_obj, "CHOOSE A NEW PET", W - 28, pet_reselect_cb, 0);
    lv_obj_set_height(pet_choose_btn, 84);
    pet_hab_x = (pet_hab_x < 16 || pet_hab_x > 92 * 16) ? 48 * 16 : pet_hab_x;
    pet_hab_mode = PET_HAB_GAZE;
    pet_hab_until = pet_now() + 1200;
    pet_refresh();
    pet_hab_render();
    if (pet_hab_timer) lv_timer_delete(pet_hab_timer);
    pet_hab_timer = lv_timer_create(pet_hab_tick, 80, NULL);
    if (replace) page_replace(p, true); else page_push(p);
}

static void pet_name_page(void);
static void pet_choose_cb(lv_event_t *e)
{
    pet_pending_species = (enum sl_pet_species)(intptr_t)lv_event_get_user_data(e);
    sfx_play(SFX_PET_SELECT);
    pet_name_page();
}

static void pet_select_page(bool replace)
{
    lv_obj_t *p, *body = page_new("Choose a Pet", &p);
    pet_page_style(p);
    lv_obj_t *intro = text(body, "PICK YOUR LITTLE FRIEND", &ss_semibold_23, PET_LIGHT);
    lv_obj_set_width(intro, lv_pct(100));
    lv_obj_set_style_text_align(intro, LV_TEXT_ALIGN_CENTER, 0);
    if (pet.species != SL_PET_UNCHOSEN)
    {
        lv_obj_t *note = text(body, "A new pet starts a fresh life when you press START.",
                              &ss_regular_16, PET_LIGHT);
        lv_obj_set_width(note, lv_pct(100));
        lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    }
    static const enum sl_pet_species species[] = {
        SL_PET_SLOP, SL_PET_SPAGHET, SL_PET_BUN, SL_PET_BLOB, SL_PET_BOT
    };
    static const char *names[] = { "SLOP", "SPAGHET", "BUN", "BLOB", "BOT" };
    static const char *notes[] = { "Floppy-eared puppy", "Floating noodle friend",
                                   "Bouncy ears", "Soft and round", "A tiny robot" };
    for (int i = 0; i < 5; i++)
    {
        lv_obj_t *card = box(body);
        lv_obj_set_size(card, lv_pct(100), 145);
        lv_obj_set_style_bg_color(card, lv_color_hex(PET_LCD), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(card, 4, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(PET_MID), 0);
        lv_obj_t *preview = pet_canvas(card, pet_preview_buf[i], 4);
        sl_align(preview, LV_ALIGN_LEFT_MID, 22, 0);   /* the picture at the reading edge, its words beside it */
        pet_render(preview, pet_preview_buf[i], 4, species[i], SL_PET_GROWN, PET_IDLE, 1);
        sl_place(text(card, names[i], &ss_semibold_26, PET_DARK), 120, 37);
        sl_place(text(card, notes[i], &ss_regular_18, PET_DARK), 120, 75);
        on_click(card, pet_choose_cb, (void *)(intptr_t)species[i]);
    }
    if (replace) page_replace(p, false); else page_push(p);
}

static void pet_name_done_cb(lv_event_t *e)
{
    (void)e;
    const char *entered = sl_ta_text(pet_name_input);
    while (*entered == ' ') entered++;
    if (!*entered) { toast("Give your pet a name first."); return; }
    if (pet.species != SL_PET_UNCHOSEN) sl_pet_init(&pet, pet_clock());
    pet_ending_set(NULL);                /* a fresh life: the old grave's words go */
    sl_pet_choose(&pet, pet_pending_species, entered, pet_clock());
    if (!pet_save()) toast_warn("Could not save your pet. Check card space.");
    pet_motion = PET_CELEBRATE;
    pet_motion_until = sl_ms() + 2200;
    sfx_play(SFX_PET_WIN);
    pet_page_core(true);
}

static void pet_name_back_cb(lv_event_t *e)
{ (void)e; pet_select_page(true); }

static void pet_name_page(void)
{
    lv_obj_t *p, *body = page_new("Name Your Pet", &p);
    pet_page_style(p);
    lv_obj_set_height(body, H - SB_H - 66 - WF_KB_H);
    lv_obj_set_style_pad_bottom(body, 12, 0);
    lv_obj_t *prompt = text(body, "WHAT SHALL WE CALL YOUR FRIEND?", &ss_semibold_21, PET_LIGHT);
    lv_obj_set_width(prompt, lv_pct(100));
    lv_obj_set_style_text_align(prompt, LV_TEXT_ALIGN_CENTER, 0);
    pet_name_input = lv_textarea_create(body);
    lv_obj_set_size(pet_name_input, lv_pct(100), 62);
    lv_textarea_set_one_line(pet_name_input, true);
    lv_textarea_set_max_length(pet_name_input, SL_PET_NAME_SIZE - 1);
    lv_textarea_set_placeholder_text(pet_name_input, "Pet name");
    lv_textarea_set_text(pet_name_input, pet_pending_species == SL_PET_SLOP ? "Slop" :
                         pet_pending_species == SL_PET_SPAGHET ? "Spaghet" : "");
    lv_obj_set_style_bg_color(pet_name_input, lv_color_hex(PET_LCD), 0);
    lv_obj_set_style_text_color(pet_name_input, lv_color_hex(PET_DARK), 0);
    lv_obj_t *buttons = box(body);
    lv_obj_set_size(buttons, lv_pct(100), 60);
    pet_button(buttons, "CHANGE", 192, pet_name_back_cb, 0);
    lv_obj_t *start = pet_button(buttons, "START", 192, pet_name_done_cb, 0);
    lv_obj_align(start, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_state(pet_name_input, LV_STATE_FOCUSED);     /* the cursor shows where letters go */
    lv_textarea_set_cursor_click_pos(pet_name_input, true);
    /* the system keyboard: big keys, no dead gaps, a bubble over the key
     * under the finger, typing never taken for a back-swipe; in the pet's
     * LCD greens */
    struct kb *kb = kb_attach(p, pet_name_input, KB_NAME, pet_name_done_cb, "START");
    kb_colours(kb, PET_MID, PET_LCD, PET_DARK, PET_DARK, 4);
    page_replace(p, true);
}

static void pet_page(void)
{
    if (!pet_page_obj && !sl_app_room(1000, "Pocket Pet")) return;
    pet_load();
    bool changed = sl_pet_advance(&pet, pet_clock());
    int joy, energy;
    bool paid = sl_pet_music_claim(&pet, &joy, &energy) > 0;
    if ((changed || paid) && !pet_save()) sl_log("pet: could not save on open");
    if (pet.species == SL_PET_UNCHOSEN) { pet_select_page(false); return; }
    /* back after a while (or for the first time since the player started):
     * it got up to something */
    uint32_t now = pet_clock();
    bool away = !pet_seen_epoch || (now >= SL_PET_VALID_CLOCK && now - pet_seen_epoch >= 2 * 3600u);
    pet_seen_epoch = now ? now : 1;
    int ev = away && !sl_pet_is_egg(&pet) ? sl_pet_away(&pet, pet_roll()) : -1;
    if (ev >= 0 && !pet_save()) sl_log("pet: could not save the away event");
    pet_page_core(false);
    if (ev >= 0 && !joy && !energy)
    {
        char b[80];
        snprintf(b, sizeof b, "WHILE YOU WERE AWAY: %s", pet_away_lines[ev]);
        toast(b);
        return;
    }
    if (!joy && !energy) return;
    char b[64];
    if (joy && energy) snprintf(b, sizeof b, "MUSIC TOGETHER! FUN +%d  ENERGY +%d", joy, energy);
    else if (joy) snprintf(b, sizeof b, "YOU LISTENED TOGETHER! FUN +%d", joy);
    else snprintf(b, sizeof b, "DOZED OFF TO YOUR MUSIC. ENERGY +%d", energy);
    toast(b);
}

#include "sloppy_pet_games.inc"

static void pet_game_choose(int kind)
{
    pet_music_poll();
    if (kind == PET_RHYTHM && !pet_mus.playing)
    {
        toast("PLAY A SONG FIRST, THEN LET'S DANCE!");
        return;
    }
    sfx_play(SFX_PET_SELECT);
    if (kind == PET_CLUB_CHOICE)
    {
        page_pop();
        pet_pending_club = true;
        pet_begin_care(SL_PET_PLAY, "GOING CLUBBING...", PET_CLUBBING, 4500);
    }
    else pet_game_open(kind);
}

static void pet_game_choice_cb(lv_event_t *e)
{
    pet_game_choose((int)(intptr_t)lv_event_get_user_data(e));
}

static lv_obj_t *pet_game_card(lv_obj_t *body, int index, const char *title, const char *blurb,
                               const char *const *glyph, int glyph_rows, int kind)
{
    static uint16_t card_bufs[6][13 * 13 * 9];
    lv_obj_t *card = box(body);
    lv_obj_set_size(card, W - 28, 96);
    lv_obj_set_style_margin_bottom(card, 10, 0);
    pet_tactile(card, PET_MID, 0x0d170f);
    on_click(card, pet_game_choice_cb, (void *)(intptr_t)kind);
    lv_obj_t *badge = box(card);
    lv_obj_set_size(badge, 66, 66);
    lv_obj_align(badge, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_set_style_bg_color(badge, lv_color_hex(PET_LCD), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(badge, SL_R_MD, 0);
    lv_obj_set_style_border_width(badge, 2, 0);
    lv_obj_set_style_border_color(badge, lv_color_hex(PET_DARK), 0);
    pet_inks_init();
    pet_target(card_bufs[index], 13, 13, 3);
    pet_px(0, 0, 13, 13, 0);
    pet_glyph(glyph, glyph_rows, 13 - (int)strlen(glyph[0]) > 0 ? (13 - (int)strlen(glyph[0])) / 2 : 0,
              (13 - glyph_rows) / 2, 0);
    lv_obj_t *icon = lv_canvas_create(badge);
    lv_canvas_set_buffer(icon, card_bufs[index], 39, 39, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(icon);
    lv_obj_t *name = text(card, title, &ss_semibold_23, PET_LIGHT);
    lv_obj_set_pos(name, 94, 16);
    lv_obj_t *about = text(card, blurb, &ss_regular_16, PET_LCD);
    lv_obj_set_pos(about, 94, 50);
    return card;
}

static void pet_game_menu(void)
{
    pet_music_poll();
    static const char *const bug_glyph[9] = {
        "#.......#", ".#.....#.", "..#...#..", "++.###.++", "+++#o#+++",
        "++#ooo#++", "..#o#o#..", "..#ooo#..", "...###..." };
    static const char *const cup_glyph[12] = {
        "....#####....", "...#+ooo##...", "...#+ooo##...", "..#+oooo###..",
        "..#+ooooo##..", ".#+oooooo###.", ".#+oooooo###.", "#############",
        "#+++++++++++#", "#############", ".....###.....", "....#+++#...." };
    static const char *const arrows[9] = {
        "..#...#..", ".##...##.", "#########", ".##...##.", "..#...#..",
        ".........", "...###...", "..#ooo#..", "..#####.." };
    static const char *const disco[11] = {
        "....###....", "...#+o+#...", "..#o+o+o#..", ".#+o+o+o+#.", ".#o+o+o+o#.",
        ".#+o+o+o+#.", "..#o+o+o#..", "...#+o+#...", "....###....", "..+.....+..", ".+.......+." };
    lv_obj_t *p, *body = page_new("Games", &p);
    pet_page_style(p);
    lv_obj_t *intro = text(body, "PLAY TO FILL FUN", &ss_semibold_23, PET_LIGHT);
    lv_obj_set_width(intro, lv_pct(100));
    lv_obj_set_style_text_align(intro, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_bottom(intro, 8, 0);
    static const char *const beat_glyph[11] = {
        ".....##....", ".....#o#...", ".....#.o#..", ".....#..#..", ".....#.....",
        "...###.....", "..#ooo#....", "..#ooo#....", "...###.....", "...........",
        "#.#.#.#.#.#" };
    pet_game_card(body, 5, "DANCE WITH ME", pet_mus.playing ? "Tap to the beat of this song" :
                  "Play a song, then dance", beat_glyph, 11, PET_RHYTHM);
    pet_game_card(body, 0, "CATCH BUG", "Swat the buzzing bug", bug_glyph, 9, PET_CATCH);
    pet_game_card(body, 1, "MELODY GEMS", "Repeat the glowing song", pet_gem_star, 12, PET_MEMORY);
    pet_game_card(body, 2, "FIND THE BALL", "Follow the pearl's cup", cup_glyph, 12, PET_CUPS);
    pet_game_card(body, 3, "LEFT OR RIGHT", "The classic guessing game", arrows, 9, PET_HEADING);
    pet_game_card(body, 4, "GO CLUBBING", "A wild night out", disco, 11, PET_CLUB_CHOICE);
    page_push(p);
}

static void pet_tick(void)
{
    pet_music_poll();
    if (pet_care_locked)
    {
        if ((int32_t)(sl_ms() - pet_care_until) >= 0)
        {
            bool was_hospital = pet_pending_hospital;
            bool was_club = pet_pending_club;
            bool was_drugs = pet_pending_drugs;
            bool was_relax = pet_pending_relax;
            pet_care_locked = false;
            pet_scene_close();
            if (pet_pending_farewell)
            {
                pet_pending_farewell = false;
                sl_pet_end_life(&pet);
                if (!pet_save()) sl_log("pet: could not save after farewell");
                pet_motion = PET_IDLE;
                pet_refresh();                  /* back to its screen: a stone and a ghost */
                return;
            }
            if (pet_pending_poke)
            {
                pet_pending_poke = false;
                pet_motion = PET_IDLE;
                pet_refresh();
                toast("NOTHING. IT'S STILL DEAD.");
                return;
            }
            sl_pet_advance(&pet, pet_clock());
            bool was_dead = pet.dead;
            const char *line = NULL;     /* this outcome's own words */
            if (pet_pending_club) sl_pet_club(&pet);
            else if (pet_pending_hospital)
                line = pet_hospital_lines[sl_pet_hospital_roll(&pet, pet_roll())];
            else if (pet_pending_relax)
            {
                bool crisis = pet.crisis;
                sl_pet_relax(&pet, pet_clock());
                if (crisis && pet.hungover)
                    line = pet_hangover_lines[pet_roll() % (sizeof pet_hangover_lines / sizeof *pet_hangover_lines)];
            }
            else if (pet_pending_drugs && pet_pending_sub >= 0)
                line = pet_sub_lines[pet_pending_sub][sl_pet_use(&pet, pet_pending_sub, pet_roll())];
            else if (pet_pending_drugs) sl_pet_take_drugs(&pet);
            else if (pet_pending_action <= SL_PET_CALM && pet_pending_action != SL_PET_PLAY)
                line = pet_care_lines[pet_pending_action][sl_pet_care_roll(&pet, pet_pending_action, pet_roll())];
            else sl_pet_care(&pet, pet_pending_action);
            if (pet.dead && !was_dead)
            {
                if (!line) line = pet_club_deaths[pet_roll() % (sizeof pet_club_deaths / sizeof *pet_club_deaths)];
                pet_ending_set(line);    /* on its grave */
                sl_log("pet: died: %s", line);
            }
            pet_pending_club = pet_pending_hospital = pet_pending_relax = pet_pending_drugs = false;
            pet_pending_sub = -1;
            bool saved = pet_save();
            pet_motion = pet.dead ? PET_IDLE : pet.hungover ? PET_HUNGOVER :
                         pet.manic ? PET_MANIC : PET_CELEBRATE;
            pet_motion_until = sl_ms() + 1700;
            pet_refresh();
            sfx_play(pet.dead ? SFX_PET_RIP : pet.manic || pet.crisis ? SFX_PET_WILD : SFX_PET_DONE);
            toast(!saved ? "Could not save your pet. Check card space." :
                  line ? line :
                  was_hospital ? pet_hospital_lines[0] :
                  was_drugs ? "EVEN WILDER! NEEDS HELP." :
                  was_relax && pet.hungover ? pet_hangover_lines[0] :
                  was_club ? "WHAT A NIGHT! FUN IS UP." :
                  pet_pending_action == SL_PET_FEED ? "YUM!" :
                  pet_pending_action == SL_PET_BATHE ? "ALL CLEAN!" :
                  pet.hungover ? "A BIT HUNGOVER TOMORROW" :
                  pet.crisis ? "YOUR PET NEEDS HELP" :
                  "ALL DONE!");
        }
        else if (pet_care_canvas)
        {
            pet_scene_render(sl_ms() - pet_care_started);
        }
    }
    if (pet_page_obj)
    {
        int joy, energy;
        bool changed = sl_pet_advance(&pet, pet_clock());
        if (!pet_care_locked && sl_pet_music_claim(&pet, &joy, &energy)) changed = true;
        if (changed)
        { if (!pet_save()) sl_log("pet: save failed after clock update"); pet_refresh(); }
        if (pet_motion != PET_IDLE && (int32_t)(sl_ms() - pet_motion_until) >= 0)
            pet_motion = PET_IDLE;
        enum sl_pet_stage stage = sl_pet_stage_at(&pet, pet_clock());
        const char *stage_text = stage == SL_PET_EGG ? "EGG" :
                                 stage == SL_PET_BABY ? "BABY" : "GROWN";
        if (strcmp(lv_label_get_text(pet_stage_label), stage_text))
        {
            if (!strcmp(lv_label_get_text(pet_stage_label), "EGG")) sfx_play(SFX_PET_HATCH);
            lv_label_set_text(pet_stage_label, stage_text);
        }
    }
}

/* The large Start tile uses the same saved character as the pet page.
 * Keep its animation independent so it continues wandering while Start is open. */
#define MT_PET_SIZE 96
static uint16_t mt_pet_pixels[MT_PET_SIZE * MT_PET_SIZE];

static void mt_pet_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!mt.page || !mt.pet_canvas || npages > 1 || overlay_visible() || mt.edit) return;
    int i = mt_find(TL_PET);
    if (i < 0) return;
    int right = mt_px(mt_span_w(mt.slot[i].size)) - MT_PET_SIZE - 12;
    enum sl_pet_stage stage = pet.species == SL_PET_UNCHOSEN ? SL_PET_EGG :
                              sl_pet_stage_at(&pet, pet_clock());
    mt.pet_frame++;
    if (stage == SL_PET_EGG)
    {   /* an egg doesn't walk: it sits in the middle and rocks now and then */
        mt.pet_x = (right + 12) / 2;
        lv_obj_set_pos(mt.pet_canvas, mt.pet_x + (mt.pet_frame % 12 < 3 ? (mt.pet_frame & 1 ? 2 : -2) : 0), 52);
        pet_render(mt.pet_canvas, mt_pet_pixels, MT_PET_SIZE / 16,
                   pet.species == SL_PET_UNCHOSEN ? SL_PET_BLOB : pet.species, SL_PET_EGG, PET_IDLE, mt.pet_frame);
        return;
    }
    mt.pet_x += mt.pet_dir * 3;
    if (mt.pet_x >= right) { mt.pet_x = right; mt.pet_dir = -1; }
    else if (mt.pet_x <= 12) { mt.pet_x = 12; mt.pet_dir = 1; }
    lv_obj_set_pos(mt.pet_canvas, mt.pet_x, 52 + (mt.pet_frame % 4 == 0 ? -2 : 0));
    enum sl_pet_species species = pet.species == SL_PET_UNCHOSEN ? SL_PET_BLOB : pet.species;
    pet_music_poll();
    enum pet_dance dance = pet.dead || stage == SL_PET_EGG ? PET_DANCE_NONE : pet_music_dance();
    enum pet_motion motion = pet.dead ? PET_SLEEPING : pet.energy < 20 ? PET_SLEEPING :
                             pet.manic ? PET_MANIC : dance == PET_DANCE_LULLED ? PET_SLEEPING :
                             dance != PET_DANCE_NONE ? PET_CLUBBING : PET_PLAYING;
    if (motion == PET_CLUBBING && pet_music_phase() < 160)
        lv_obj_set_pos(mt.pet_canvas, mt.pet_x, 46);
    pet_render(mt.pet_canvas, mt_pet_pixels, MT_PET_SIZE / 16, species, stage, motion,
               motion == PET_CLUBBING ? pet_mus.beats : mt.pet_frame);
}

static void mt_pet_fill(struct mt_slot *s)
{
    pet_load();
    lv_obj_t *tile = s->obj;
    int w = mt_px(mt_span_w(s->size));
    lv_obj_set_style_bg_color(tile, lv_color_hex(PET_LCD), 0);
    lv_obj_t *name = text(tile, pet.name[0] ? pet.name : "your pet", &ss_semibold_21, PET_DARK);
    lv_obj_set_pos(name, 12, 11);
    lv_obj_set_width(name, w - 24);
    label_lines(name, 1);
    lv_obj_t *floor = box(tile);
    lv_obj_set_pos(floor, 10, 151);
    lv_obj_set_size(floor, w - 20, 2);
    lv_obj_set_style_bg_color(floor, lv_color_hex(PET_MID), 0);
    lv_obj_set_style_bg_opa(floor, LV_OPA_COVER, 0);
    mt.pet_canvas = lv_canvas_create(tile);
    lv_canvas_set_buffer(mt.pet_canvas, mt_pet_pixels, MT_PET_SIZE, MT_PET_SIZE,
                         LV_COLOR_FORMAT_RGB565);
    mt.pet_x = 12;
    mt.pet_dir = 1;
    mt.pet_frame = 0;
    lv_obj_t *title = text(tile, "Tamagotchi", &ss_regular_18, PET_DARK);
    lv_obj_align(title, LV_ALIGN_BOTTOM_LEFT, 10, -7);
    mt_pet_tick(NULL);
}
