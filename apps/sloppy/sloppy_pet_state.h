/* Clock-driven virtual pet state. GPLv2+. No UI or filesystem dependencies. */
#ifndef SLOPPY_PET_STATE_H
#define SLOPPY_PET_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define SL_PET_MAGIC 0x35544550u /* PET5 */
#define SL_PET_V4_MAGIC 0x34544550u /* PET4 */
#define SL_PET_V3_MAGIC 0x33544550u /* PET3 */
#define SL_PET_V2_MAGIC 0x32544550u /* PET2 */
#define SL_PET_OLD_MAGIC 0x31544550u /* PET1 */
#define SL_PET_VALID_CLOCK 1577836800u
#define SL_PET_NAME_SIZE 16
#define SL_PET_ARTIST_SIZE 24
#define SL_PET_TASTE 4
#define SL_PET_BANK_MAX 600

enum sl_pet_action { SL_PET_FEED, SL_PET_BATHE, SL_PET_PLAY, SL_PET_REST, SL_PET_CALM };
enum sl_pet_species { SL_PET_UNCHOSEN, SL_PET_BUN, SL_PET_BLOB, SL_PET_BOT,
                      SL_PET_SLOP, SL_PET_SPAGHET, SL_PET_SPECIES_COUNT };
/* born_epoch of a pet still in its egg (no real clock is that small) */
#define SL_PET_UNHATCHED 1u
enum sl_pet_stage { SL_PET_EGG, SL_PET_BABY, SL_PET_GROWN };

/* Artists the pet has heard most, kept by the space-saving count: a new
 * artist takes over the quietest slot, so a real favorite keeps its place. */
struct sl_pet_taste {
    char artist[SL_PET_ARTIST_SIZE];
    uint16_t minutes;
};

struct sl_pet_state {
    uint32_t magic, last_epoch, meals, baths, games, rests, born_epoch;
    uint8_t food, clean, joy, energy, species;
    char name[SL_PET_NAME_SIZE];
    uint8_t stimulation, manic;
    uint8_t crisis, dead, hungover, neglect_hours;
    uint32_t hungover_until;
    uint32_t music_minutes, songs_heard;
    uint16_t bank_day, bank_night;      /* listening not yet paid out, minutes */
    struct sl_pet_taste taste[SL_PET_TASTE];
    uint32_t checksum;
};

struct sl_pet_v4_state {
    uint32_t magic, last_epoch, meals, baths, games, rests, born_epoch;
    uint8_t food, clean, joy, energy, species;
    char name[SL_PET_NAME_SIZE];
    uint8_t stimulation, manic;
    uint8_t crisis, dead, hungover, neglect_hours;
    uint32_t hungover_until;
    uint32_t checksum;
};

struct sl_pet_v3_state {
    uint32_t magic, last_epoch, meals, baths, games, rests, born_epoch;
    uint8_t food, clean, joy, energy, species;
    char name[SL_PET_NAME_SIZE];
    uint8_t stimulation, manic;
    uint32_t checksum;
};

struct sl_pet_v2_state {
    uint32_t magic, last_epoch, meals, baths, games, rests, born_epoch;
    uint8_t food, clean, joy, energy, species;
    char name[SL_PET_NAME_SIZE];
    uint32_t checksum;
};

/* The original PET1 layout is read only to migrate existing care progress. */
struct sl_pet_legacy_state {
    uint32_t magic, last_epoch, meals, baths, games, rests;
    uint8_t food, clean, joy, energy;
    uint32_t checksum;
};

static inline uint8_t sl_pet_clamp(int n)
{
    return n < 0 ? 0 : n > 100 ? 100 : n;
}

static inline uint32_t sl_pet_bytes_checksum(const void *data, unsigned count)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < count; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static inline uint32_t sl_pet_checksum(const struct sl_pet_state *s)
{
    return sl_pet_bytes_checksum(s, sizeof(*s) - sizeof(s->checksum));
}

static inline void sl_pet_init(struct sl_pet_state *s, uint32_t now)
{
    memset(s, 0, sizeof *s);            /* padding too: it is checksummed */
    s->magic = SL_PET_MAGIC;
    s->last_epoch = now >= SL_PET_VALID_CLOCK ? now : 0;
    s->food = s->clean = s->joy = s->energy = 85;
    s->checksum = sl_pet_checksum(s);
}

/* favourite-artist names are used as C strings */
static inline bool sl_pet_tastes_terminated(const struct sl_pet_state *s)
{
    for (int i = 0; i < SL_PET_TASTE; i++)
        if (!memchr(s->taste[i].artist, 0, SL_PET_ARTIST_SIZE)) return false;
    return true;
}

static inline bool sl_pet_valid(const struct sl_pet_state *s)
{
    return s->magic == SL_PET_MAGIC && s->food <= 100 && s->clean <= 100 &&
           s->joy <= 100 && s->energy <= 100 &&
           s->species < SL_PET_SPECIES_COUNT &&
           s->stimulation <= 100 && s->manic <= 1 &&
           s->crisis <= 1 && s->dead <= 1 && s->hungover <= 1 &&
           (!s->hungover || s->hungover_until >= SL_PET_VALID_CLOCK + 86400u) &&
           memchr(s->name, 0, sizeof(s->name)) != NULL &&
           sl_pet_tastes_terminated(s) &&
           s->bank_day <= SL_PET_BANK_MAX && s->bank_night <= SL_PET_BANK_MAX &&
           s->checksum == sl_pet_checksum(s);
}

static inline bool sl_pet_migrate_v4(struct sl_pet_state *s,
                                     const struct sl_pet_v4_state *old)
{
    if (old->magic != SL_PET_V4_MAGIC || old->food > 100 || old->clean > 100 ||
        old->joy > 100 || old->energy > 100 || old->stimulation > 100 ||
        old->manic > 1 || old->crisis > 1 || old->dead > 1 || old->hungover > 1 ||
        old->species >= SL_PET_SPECIES_COUNT ||
        memchr(old->name, 0, sizeof old->name) == NULL ||
        old->checksum != sl_pet_bytes_checksum(old, sizeof *old - sizeof old->checksum))
        return false;
    sl_pet_init(s, old->last_epoch);
    s->meals = old->meals; s->baths = old->baths;
    s->games = old->games; s->rests = old->rests;
    s->born_epoch = old->born_epoch;
    s->food = old->food; s->clean = old->clean;
    s->joy = old->joy; s->energy = old->energy;
    s->species = old->species;
    memcpy(s->name, old->name, sizeof s->name);
    s->stimulation = old->stimulation; s->manic = old->manic;
    s->crisis = old->crisis; s->dead = old->dead;
    s->hungover = old->hungover; s->neglect_hours = old->neglect_hours;
    s->hungover_until = old->hungover_until;
    s->checksum = sl_pet_checksum(s);
    return true;
}

static inline bool sl_pet_migrate_v3(struct sl_pet_state *s,
                                     const struct sl_pet_v3_state *old)
{
    if (old->magic != SL_PET_V3_MAGIC || old->food > 100 || old->clean > 100 ||
        old->joy > 100 || old->energy > 100 || old->stimulation > 100 ||
        old->manic > 1 || old->species >= SL_PET_SPECIES_COUNT ||
        memchr(old->name, 0, sizeof old->name) == NULL ||
        old->checksum != sl_pet_bytes_checksum(old, sizeof *old - sizeof old->checksum))
        return false;
    sl_pet_init(s, old->last_epoch);
    s->meals = old->meals; s->baths = old->baths;
    s->games = old->games; s->rests = old->rests;
    s->born_epoch = old->born_epoch;
    s->food = old->food; s->clean = old->clean;
    s->joy = old->joy; s->energy = old->energy;
    s->species = old->species;
    memcpy(s->name, old->name, sizeof s->name);
    s->stimulation = old->stimulation; s->manic = old->manic;
    s->checksum = sl_pet_checksum(s);
    return true;
}

static inline bool sl_pet_migrate_v2(struct sl_pet_state *s,
                                     const struct sl_pet_v2_state *old)
{
    if (old->magic != SL_PET_V2_MAGIC || old->food > 100 || old->clean > 100 ||
        old->joy > 100 || old->energy > 100 || old->species >= SL_PET_SLOP ||
        memchr(old->name, 0, sizeof(old->name)) == NULL ||
        old->checksum != sl_pet_bytes_checksum(old, sizeof(*old) - sizeof(old->checksum)))
        return false;
    sl_pet_init(s, old->last_epoch);
    s->meals = old->meals; s->baths = old->baths;
    s->games = old->games; s->rests = old->rests;
    s->born_epoch = old->born_epoch;
    s->food = old->food; s->clean = old->clean;
    s->joy = old->joy; s->energy = old->energy;
    s->species = old->species;
    memcpy(s->name, old->name, sizeof(s->name));
    s->checksum = sl_pet_checksum(s);
    return true;
}

static inline bool sl_pet_migrate(struct sl_pet_state *s,
                                  const struct sl_pet_legacy_state *old)
{
    if (old->magic != SL_PET_OLD_MAGIC || old->food > 100 || old->clean > 100 ||
        old->joy > 100 || old->energy > 100 ||
        old->checksum != sl_pet_bytes_checksum(old, sizeof(*old) - sizeof(old->checksum)))
        return false;
    sl_pet_init(s, old->last_epoch);
    s->meals = old->meals; s->baths = old->baths;
    s->games = old->games; s->rests = old->rests;
    s->food = old->food; s->clean = old->clean;
    s->joy = old->joy; s->energy = old->energy;
    s->checksum = sl_pet_checksum(s);
    return true;
}

static inline void sl_pet_choose(struct sl_pet_state *s, enum sl_pet_species species,
                                  const char *name, uint32_t now)
{
    if (species <= SL_PET_UNCHOSEN || species >= SL_PET_SPECIES_COUNT) return;
    s->species = species;
    unsigned i = 0;
    while (name && name[i] && i < SL_PET_NAME_SIZE - 1) { s->name[i] = name[i]; i++; }
    if (name && name[i] && ((unsigned char)name[i] & 0xc0) == 0x80)
        while (i && ((unsigned char)name[i] & 0xc0) == 0x80) i--;   /* not a letter cut in half */
    s->name[i] = 0;
    s->born_epoch = SL_PET_UNHATCHED;       /* it starts as an egg; tapping hatches it */
    (void)now;
    s->checksum = sl_pet_checksum(s);
}

static inline bool sl_pet_is_egg(const struct sl_pet_state *s)
{
    return s->species != SL_PET_UNCHOSEN && s->born_epoch == SL_PET_UNHATCHED;
}

/* The egg cracks open. Needs start counting from now: an egg doesn't get
 * hungry, however long it waited. */
static inline void sl_pet_hatch(struct sl_pet_state *s, uint32_t now)
{
    if (!sl_pet_is_egg(s)) return;
    bool clock = now >= SL_PET_VALID_CLOCK;
    s->born_epoch = clock ? now : 0;
    if (clock) s->last_epoch = now;
    s->checksum = sl_pet_checksum(s);
}

static inline enum sl_pet_stage sl_pet_stage_at(const struct sl_pet_state *s, uint32_t now)
{
    if (s->born_epoch == SL_PET_UNHATCHED) return SL_PET_EGG;
    if (!s->born_epoch || now < s->born_epoch) return SL_PET_BABY;
    return now - s->born_epoch < 12 * 3600 ? SL_PET_BABY : SL_PET_GROWN;
}

/* Whole hours keep decay deterministic across restarts and timer intervals. */
static inline bool sl_pet_advance(struct sl_pet_state *s, uint32_t now)
{
    if (now < SL_PET_VALID_CLOCK) return false;
    if (sl_pet_is_egg(s)) return false;     /* nothing inside needs care yet */
    if (!s->last_epoch) { s->last_epoch = now; s->checksum = sl_pet_checksum(s); return true; }
    if (now + 86400u < s->last_epoch)
    {
        /* The clock went back more than a day (a reset or corrected RTC).
         * Waiting for it to catch up would freeze the pet for that long:
         * start counting from the new time instead. */
        s->last_epoch = now;
        s->checksum = sl_pet_checksum(s);
        return true;
    }
    if (now <= s->last_epoch) return false;
    uint32_t hours = (now - s->last_epoch) / 3600u;
    if (!hours) return false;
    uint32_t decay = hours > 100 ? 100 : hours;
    s->food = sl_pet_clamp((int)s->food - (int)decay * 3);
    s->clean = sl_pet_clamp((int)s->clean - (int)decay);
    s->joy = sl_pet_clamp((int)s->joy - (int)decay * 2);
    s->energy = sl_pet_clamp((int)s->energy - (int)decay);
    s->stimulation = sl_pet_clamp((int)s->stimulation - (int)decay * 4);
    if (s->hungover && s->last_epoch < s->hungover_until - 24 * 3600u &&
        now >= s->hungover_until - 24 * 3600u)
        s->energy = sl_pet_clamp((int)s->energy - 15);
    if (s->hungover && now >= s->hungover_until) s->hungover = 0;
    if (!s->dead && s->species != SL_PET_UNCHOSEN && s->food == 0 && s->joy == 0)
    {
        unsigned neglected = s->neglect_hours + hours;
        s->neglect_hours = neglected > 255 ? 255 : neglected;
        if (s->neglect_hours >= 48) s->dead = 1;
    }
    else s->neglect_hours = 0;
    s->last_epoch += hours * 3600u;
    s->checksum = sl_pet_checksum(s);
    return true;
}

static inline void sl_pet_stimulate(struct sl_pet_state *s, int amount)
{
    if (s->joy >= 75 || s->manic)
    {
        s->stimulation = sl_pet_clamp((int)s->stimulation + amount);
        if (s->stimulation >= 80) s->manic = 1;
    }
}

static inline void sl_pet_care(struct sl_pet_state *s, enum sl_pet_action action)
{
    if (s->dead) return;
    switch (action) {
    case SL_PET_FEED:
        s->food = sl_pet_clamp(s->food + 28); s->meals++; s->neglect_hours = 0;
        sl_pet_stimulate(s, 18); break;
    case SL_PET_BATHE:
        s->clean = sl_pet_clamp(s->clean + 35); s->baths++;
        sl_pet_stimulate(s, 14); break;
    case SL_PET_PLAY: /* Games grant Fun when a round finishes. */ break;
    case SL_PET_REST:
        s->energy = sl_pet_clamp(s->energy + 35); s->rests++;
        s->stimulation = sl_pet_clamp((int)s->stimulation - 12); break;
    case SL_PET_CALM:
        s->stimulation = sl_pet_clamp((int)s->stimulation - 42);
        s->energy = sl_pet_clamp(s->energy + 5);
        if (s->stimulation <= 20) s->manic = 0;
        break;
    }
    s->checksum = sl_pet_checksum(s);
}

/* Who it is: fixed at birth from its birthday and name, no field of its
 * own (an old save keeps the pet it was). It leans every outcome below. */
enum sl_pet_nature { SL_PET_GLUTTON, SL_PET_CLEAN_FREAK, SL_PET_PARTY_ANIMAL, SL_PET_SLEEPYHEAD,
                     SL_PET_MOODY, SL_PET_NATURES };
static inline int sl_pet_nature(const struct sl_pet_state *s)
{
    uint32_t h = s->born_epoch * 2654435761u;
    for (const char *c = s->name; *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
    return (int)(h % SL_PET_NATURES);
}

/* Care that doesn't always go the same way: 0 is the plain outcome, the
 * others lean on the pet's nature and how it is doing. Returns which. */
#define SL_PET_CARE_OUTCOMES 4
static inline int sl_pet_care_roll(struct sl_pet_state *s, enum sl_pet_action action, unsigned roll)
{
    if (s->dead) return 0;
    int nat = sl_pet_nature(s), r = (int)(roll % 100), o = 0;
    switch (action) {
    case SL_PET_FEED:
        if (r < (nat == SL_PET_GLUTTON ? 35 : 12) + (s->food < 30 ? 20 : 0)) o = 1;          /* devoured */
        else if (r >= 100 - ((nat == SL_PET_MOODY ? 22 : 8) + (s->food > 80 ? 20 : 0))) o = 2; /* picky */
        else if (nat == SL_PET_GLUTTON && s->food > 60 && r % 3 == 0) o = 3;                 /* food coma */
    {
        int before = s->food;
        sl_pet_care(s, SL_PET_FEED);
        if (o == 1) { s->food = sl_pet_clamp(s->food + 12); s->joy = sl_pet_clamp(s->joy + 10); }
        else if (o == 2) { s->food = sl_pet_clamp(before + 10); s->joy = sl_pet_clamp((int)s->joy - 5); }   /* a bite or two */
    }
        if (o == 3) s->energy = sl_pet_clamp((int)s->energy - 15);
        break;
    case SL_PET_BATHE:
        if (r < (nat == SL_PET_CLEAN_FREAK ? 40 : 12)) o = 1;                                 /* loved it */
        else if (r >= 100 - (nat == SL_PET_PARTY_ANIMAL || nat == SL_PET_MOODY ? 28 : 10)) o = 2; /* hated it */
        else if (r % 7 == 0) o = 3;                                                          /* splash war */
    {
        int before = s->clean;
        sl_pet_care(s, SL_PET_BATHE);
        if (o == 1) { s->clean = sl_pet_clamp(s->clean + 10); s->joy = sl_pet_clamp(s->joy + 10); }
        else if (o == 2) { s->clean = sl_pet_clamp(before + 20); s->joy = sl_pet_clamp((int)s->joy - 10); }   /* half a wash */
    }
        if (o == 3) { s->joy = sl_pet_clamp(s->joy + 15); s->energy = sl_pet_clamp((int)s->energy - 10); }
        break;
    case SL_PET_REST:
        if (r < (nat == SL_PET_SLEEPYHEAD ? 40 : 15)) o = 1;                                  /* deep sleep */
        else if (r >= 100 - (s->stimulation > 50 || nat == SL_PET_MOODY ? 25 : 8)) o = 2;    /* nightmare */
        else if (r % 9 == 0) o = 3;                                                          /* sleepwalked */
    {
        int before = s->energy;
        sl_pet_care(s, SL_PET_REST);
        if (o == 1) s->energy = sl_pet_clamp(s->energy + 15);
        else if (o == 2) { s->energy = sl_pet_clamp(before + 15); s->joy = sl_pet_clamp((int)s->joy - 10); }
        else if (o == 3) { s->energy = sl_pet_clamp(before + 25); s->food = sl_pet_clamp((int)s->food - 10); }
    }
        break;
    case SL_PET_CALM:
        if (r < 20) o = 1;                                                                   /* zen */
        else if (r >= 100 - (nat == SL_PET_PARTY_ANIMAL ? 30 : 12)) o = 2;                   /* restless */
        sl_pet_care(s, SL_PET_CALM);
        if (o == 1) s->stimulation = sl_pet_clamp((int)s->stimulation - 20);
        else if (o == 2) s->stimulation = sl_pet_clamp((int)s->stimulation + 25);
        if (s->stimulation <= 20) s->manic = 0;
        break;
    default:
        sl_pet_care(s, action);
    }
    s->checksum = sl_pet_checksum(s);
    return o;
}

/* While you were away, something happened; which one leans on its nature. */
#define SL_PET_AWAY_EVENTS 8
static inline int sl_pet_away(struct sl_pet_state *s, unsigned roll)
{
    if (s->dead || s->species == SL_PET_UNCHOSEN) return -1;
    int nat = sl_pet_nature(s);
    /* half the events go with its nature, the rest anything */
    static const int8_t lean[SL_PET_NATURES] = { 3, 1, 2, 4, 5 };
    int e = roll % 2 ? lean[nat] : (int)((roll >> 1) % SL_PET_AWAY_EVENTS);
    switch (e) {
    case 0: s->joy = sl_pet_clamp(s->joy + 5); break;                                        /* a shiny coin */
    case 1: s->clean = sl_pet_clamp(s->clean + 10); break;                                   /* tidied up */
    case 2: s->joy = sl_pet_clamp(s->joy + 10); s->energy = sl_pet_clamp((int)s->energy - 10); break; /* a new dance */
    case 3: s->food = sl_pet_clamp(s->food + 15); s->clean = sl_pet_clamp((int)s->clean - 5); break; /* the fridge */
    case 4: s->energy = sl_pet_clamp(s->energy + 10); break;                                 /* napped */
    case 5: s->joy = sl_pet_clamp((int)s->joy - 5); break;                                   /* a sad poem */
    case 6: s->clean = sl_pet_clamp((int)s->clean - 10); break;                              /* the plant */
    default: break;                                                                          /* a weird dream */
    }
    s->checksum = sl_pet_checksum(s);
    return e;
}

static inline void sl_pet_take_drugs(struct sl_pet_state *s)
{
    if (!s->manic || s->dead) return;
    s->crisis = 1; s->stimulation = 100;
    s->energy = sl_pet_clamp((int)s->energy - 25);
    s->checksum = sl_pet_checksum(s);
}

/* What a manic pet can take, and one more when it is already overwhelmed.
 * sl_pet_use applies one and says which way it went (roll: any random
 * number); the UI has a line for every outcome. ONE MORE BUMP on an
 * overwhelmed pet is the overdose: usually its last. */
enum sl_pet_substance { SL_PET_SNORT, SL_PET_ENERGY, SL_PET_JOINT, SL_PET_ACID, SL_PET_SYRUP,
                        SL_PET_BUMP, SL_PET_SUBSTANCES };
#define SL_PET_OUTCOMES 5
static const uint8_t sl_pet_outcomes[SL_PET_SUBSTANCES] = { 3, 3, 3, 4, 3, 5 };

static inline int sl_pet_use(struct sl_pet_state *s, int what, unsigned roll)
{
    if (s->dead || what < 0 || what >= SL_PET_SUBSTANCES) return 0;
    int o = (int)(roll % sl_pet_outcomes[what]);
    /* how it is doing leans the dice: a happy pet trips well, a worn-out
     * one rarely survives one more */
    if (what == SL_PET_ACID && s->joy >= 70 && o == 1 && (roll / 7) % 2) o = 0;
    if (what == SL_PET_ACID && s->joy < 30 && o == 0 && (roll / 7) % 2) o = 1;
    if (what == SL_PET_BUMP && s->energy < 30 && o >= 3 && (roll / 7) % 2) o = 0;
    if (what == SL_PET_BUMP && s->energy >= 70 && o < 3 && (roll / 7) % 3 == 0) o = 3;
    switch (what) {
    case SL_PET_SNORT:                   /* straight to a crisis, every time */
        s->manic = 1; s->crisis = 1; s->stimulation = 100;
        s->energy = sl_pet_clamp((int)s->energy - 25);
        break;
    case SL_PET_ENERGY:                  /* wired, awake, and one time in three, too much */
        s->manic = 1; s->stimulation = 100;
        s->energy = sl_pet_clamp((int)s->energy + 15);
        if (o == 2) s->crisis = 1;
        break;
    case SL_PET_JOINT:                   /* down off the ceiling, but... */
        s->manic = 0; s->stimulation = 15;
        if (o == 0) s->food = sl_pet_clamp((int)s->food - 30);        /* the munchies */
        else if (o == 1) s->energy = sl_pet_clamp((int)s->energy - 15);
        else s->joy = sl_pet_clamp((int)s->joy - 20);                /* paranoid */
        break;
    case SL_PET_ACID:
        if (o == 0) { s->manic = 0; s->stimulation = 30; s->joy = 100; }   /* enlightened */
        else if (o == 1) { s->manic = 1; s->crisis = 1; s->stimulation = 100; } /* bad trip */
        else if (o == 2) { s->manic = 0; s->stimulation = 20; s->energy = sl_pet_clamp((int)s->energy - 30); }
        else { s->joy = sl_pet_clamp((int)s->joy + 15); s->stimulation = 90; }
        break;
    case SL_PET_SYRUP:                   /* everything slows right down */
        s->manic = 0; s->stimulation = 5;
        s->energy = sl_pet_clamp((int)s->energy - 40);
        break;
    case SL_PET_BUMP:                    /* already overwhelmed: three in five, the end */
        if (!s->crisis) { o = 3; break; }
        if (o < 3) { s->dead = 1; break; }
        s->energy = sl_pet_clamp((int)s->energy - 25); s->stimulation = 100;
        break;
    }
    s->checksum = sl_pet_checksum(s);
    return o;
}

static inline void sl_pet_relax(struct sl_pet_state *s, uint32_t now)
{
    if (s->dead) return;
    /* No hangover without a real clock: sl_pet_valid rejects a hangover
     * ending before SL_PET_VALID_CLOCK, so the next load would reset the pet. */
    if (s->crisis && now >= SL_PET_VALID_CLOCK)
    {
        s->hungover = 1; s->hungover_until = now + 48 * 3600u;
    }
    s->crisis = s->manic = 0; s->stimulation = 10;
    s->checksum = sl_pet_checksum(s);
}

#define SL_PET_HOSPITAL_OUTCOMES 5
/* Out of the crisis, one way or another; returns which (roll: random). */
static inline int sl_pet_hospital_roll(struct sl_pet_state *s, unsigned roll)
{
    if (!s->crisis || s->dead) return 0;
    int o = (int)(roll % SL_PET_HOSPITAL_OUTCOMES);
    s->crisis = s->manic = s->hungover = 0;
    s->stimulation = 0; s->energy = 90; s->joy = 75; s->food = 75;
    s->neglect_hours = 0;
    if (o == 2) { s->manic = 1; s->stimulation = 70; s->energy = 60; }   /* left early */
    else if (o == 3) s->joy = 20;                                       /* the bill */
    else if (o == 4) s->joy = 100;                                      /* the nurse */
    s->checksum = sl_pet_checksum(s);
    return o;
}
static inline void sl_pet_hospital(struct sl_pet_state *s) { sl_pet_hospital_roll(s, 0); }

static inline void sl_pet_end_life(struct sl_pet_state *s)
{
    s->dead = 1;
    s->checksum = sl_pet_checksum(s);
}

/* A manic or overwhelmed pet can keep partying, but every round costs extra
 * energy; running it to empty while wired is fatal. */
static inline int sl_pet_wired_cost(const struct sl_pet_state *s)
{
    return s->crisis ? 12 : s->manic ? 7 : 0;
}

static inline void sl_pet_exhaust(struct sl_pet_state *s, bool wired)
{
    if (wired && s->energy == 0) s->dead = 1;
}

static inline void sl_pet_club(struct sl_pet_state *s)
{
    if (s->dead) return;
    bool wired = s->manic || s->crisis;
    s->joy = sl_pet_clamp((int)s->joy + 30);
    s->energy = sl_pet_clamp((int)s->energy - 18 - sl_pet_wired_cost(s));
    s->games++;
    sl_pet_stimulate(s, 42);
    sl_pet_exhaust(s, wired);
    s->checksum = sl_pet_checksum(s);
}

/* A cuddle soothes a little every time; the UI rate-limits the joy reward. */
static inline void sl_pet_cuddle(struct sl_pet_state *s, bool reward)
{
    if (s->dead) return;
    s->stimulation = sl_pet_clamp((int)s->stimulation - 4);
    if (reward) s->joy = sl_pet_clamp(s->joy + 2);
    s->checksum = sl_pet_checksum(s);
}

/* Music heard together, one minute at a time. Songs the pet loves (its
 * species' favorite style, or its favorite artist) bank double. Daytime
 * minutes pay out as Fun, late-night minutes as Energy (it dozed off to them). */
static inline void sl_pet_listen(struct sl_pet_state *s, const char *artist,
                                 bool night, bool loved)
{
    if (s->dead || s->species == SL_PET_UNCHOSEN) return;
    uint16_t *bank = night ? &s->bank_night : &s->bank_day;
    unsigned banked = *bank + (loved ? 2u : 1u);
    *bank = banked > SL_PET_BANK_MAX ? SL_PET_BANK_MAX : banked;
    s->music_minutes++;
    if (artist && artist[0])
    {
        int hit = -1, low = 0;
        for (int i = 0; i < SL_PET_TASTE && hit < 0; i++)
        {
            if (!strncmp(s->taste[i].artist, artist, SL_PET_ARTIST_SIZE - 1)) hit = i;
            else if (s->taste[i].minutes < s->taste[low].minutes) low = i;
        }
        if (hit < 0)
        {
            /* space saving: the newcomer inherits the evicted count */
            hit = low;
            strncpy(s->taste[hit].artist, artist, SL_PET_ARTIST_SIZE - 1);
            s->taste[hit].artist[SL_PET_ARTIST_SIZE - 1] = 0;
        }
        struct sl_pet_taste *slot = &s->taste[hit];
        if (slot->minutes < 65535) slot->minutes++;
    }
    s->checksum = sl_pet_checksum(s);
}

static inline void sl_pet_heard_song(struct sl_pet_state *s)
{
    if (s->dead || s->species == SL_PET_UNCHOSEN) return;
    s->songs_heard++;
    s->checksum = sl_pet_checksum(s);
}

/* The favorite needs ten minutes and a clear lead over the runner-up. */
static inline const char *sl_pet_favorite(const struct sl_pet_state *s)
{
    int best = -1, second = 0;
    for (int i = 0; i < SL_PET_TASTE; i++)
    {
        int m = s->taste[i].minutes;
        if (best < 0 || m > s->taste[best].minutes)
        { if (best >= 0) second = s->taste[best].minutes; best = i; }
        else if (m > second) second = m;
    }
    return best >= 0 && s->taste[best].minutes >= 10 &&
           s->taste[best].minutes > second && s->taste[best].artist[0] ?
           s->taste[best].artist : NULL;
}

/* Pay whole five-minute blocks: 2 Fun or 2 Energy each, up to 12 of each per
 * claim. Never adds excitement, so music alone cannot tip a pet into mania. */
static inline unsigned sl_pet_music_claim(struct sl_pet_state *s, int *joy, int *energy)
{
    unsigned day = s->bank_day / 5, night = s->bank_night / 5;
    *joy = *energy = 0;
    if (!day && !night) return 0;
    s->bank_day -= day * 5; s->bank_night -= night * 5;
    if (!s->dead)
    {
        int j = s->joy, e = s->energy;
        s->joy = sl_pet_clamp(s->joy + (int)(day * 2 > 12 ? 12 : day * 2));
        s->energy = sl_pet_clamp(s->energy + (int)(night * 2 > 12 ? 12 : night * 2));
        *joy = s->joy - j; *energy = s->energy - e;
    }
    s->checksum = sl_pet_checksum(s);
    return (day + night) * 5;
}

static inline void sl_pet_game_result(struct sl_pet_state *s, bool won)
{
    if (s->dead) return;
    bool wired = s->manic || s->crisis;
    s->joy = sl_pet_clamp(s->joy + (won ? 24 : 8));
    s->energy = sl_pet_clamp(s->energy - (won ? 9 : 5) - sl_pet_wired_cost(s));
    s->clean = sl_pet_clamp(s->clean - 3);
    s->games++;
    sl_pet_stimulate(s, won ? 28 : 16);
    sl_pet_exhaust(s, wired);
    s->checksum = sl_pet_checksum(s);
}

#endif
