/**
 * @file lv_font.c
 *
 */

/*********************
 *      INCLUDES
 *********************/

#include "lv_font.h"
#include "../misc/lv_text_private.h"
#include "../misc/lv_utils.h"
#include "../misc/lv_log.h"
#include "../misc/lv_assert.h"
#include "../stdlib/lv_string.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

const void * lv_font_get_glyph_bitmap(lv_font_glyph_dsc_t * g_dsc, lv_draw_buf_t * draw_buf)
{
    const lv_font_t * font_p = g_dsc->resolved_font;
    LV_ASSERT_NULL(font_p);
    return font_p->get_glyph_bitmap(g_dsc, draw_buf);
}

void lv_font_glyph_release_draw_data(lv_font_glyph_dsc_t * g_dsc)
{
    const lv_font_t * font = g_dsc->resolved_font;

    if(font != NULL && font->release_glyph) {
        font->release_glyph(font, g_dsc);
    }
}

/* GabagoolOS: the letter to draw for one no font has, or 0 for none.
 * Dashes, spaces, quotes and fullwidth ASCII have a near twin in every font. */
static uint32_t near_letter(uint32_t c)
{
    switch(c) {
        case 0x2010: case 0x2011: case 0x2012: case 0x2043: case 0x2212: case 0xFE63: case 0xFF0D:
        case 0x2013: return '-';
        case 0x2015: case 0x2E3A: case 0x2E3B: case 0xFE58: return 0x2014;
        case 0x2014: return 0x2013;
        case 0x00A0: case 0x202F: case 0x205F: case 0x3000: return ' ';
        case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2032: case 0x2035: case 0x02B9: case 0x02BC:
        case 0x2039: case 0x203A: return 0x2039 == c ? '<' : 0x203A == c ? '>' : '\'';
        case 0x201C: case 0x201D: case 0x201E: case 0x201F: case 0x2033: case 0x2036: case 0x02BA: case 0x00AB:
        case 0x00BB: return '"';
        case 0x2026: case 0x2024: return '.';
        case 0x2022: case 0x2027: case 0x2219: case 0x22C5: return 0x00B7;
        case 0x00B7: return '.';
        case 0x2044: case 0x2215: return '/';
        case 0x2217: return '*';
        case 0x2223: return '|';
        case 0x2236: return ':';
        case 0x223C: case 0x301C: return '~';
        case 0x2030: return '%';
    }
    if(c >= 0x2000 && c <= 0x200A) return ' ';
    if(c >= 0xFF01 && c <= 0xFF5E) return c - 0xFEE0;
    return 0;
}

/* Control and format letters take no room when no font draws them. */
static bool no_room(uint32_t c)
{
    return c < 0x20 || (c >= 0x7F && c <= 0x9F) || c == 0xAD || c == 0x34F || c == 0x61C || c == 0x180E ||
           (c >= 0x200B && c <= 0x200F) || (c >= 0x2028 && c <= 0x202E) || (c >= 0x2060 && c <= 0x206F) ||
           (c >= 0xFE00 && c <= 0xFE0F) || c == 0xFEFF || (c >= 0xFFF9 && c <= 0xFFFB) ||
           (c >= 0xE0000 && c <= 0xE01EF);
}

bool lv_font_get_glyph_dsc(const lv_font_t * font_p, lv_font_glyph_dsc_t * dsc_out, uint32_t letter,
                           uint32_t letter_next)
{

    LV_ASSERT_NULL(font_p);
    LV_ASSERT_NULL(dsc_out);

#if LV_USE_FONT_PLACEHOLDER
    const lv_font_t * placeholder_font = NULL;
#endif

    const lv_font_t * f = font_p;

    dsc_out->resolved_font = NULL;

    while(f) {
        bool found = f->get_glyph_dsc(f, dsc_out, letter, f->kerning == LV_FONT_KERNING_NONE ? 0 : letter_next);
        if(found) {
            if(!dsc_out->is_placeholder) {
                dsc_out->resolved_font = f;
                return true;
            }
#if LV_USE_FONT_PLACEHOLDER
            else if(placeholder_font == NULL) {
                placeholder_font = f;
            }
#endif
        }
        f = f->fallback;
    }

#if LV_USE_FONT_PLACEHOLDER
    if(placeholder_font != NULL) {
        placeholder_font->get_glyph_dsc(placeholder_font, dsc_out, letter,
                                        placeholder_font->kerning == LV_FONT_KERNING_NONE ? 0 : letter_next);
        dsc_out->resolved_font = placeholder_font;
        return true;
    }
#endif

    /* GabagoolOS: never a box. Its nearest letter, else a thin gap (nothing
     * for a control or format letter). Still false: the audit lists it. */
    uint32_t near = near_letter(letter);
    if(near) return lv_font_get_glyph_dsc(font_p, dsc_out, near, letter_next);

#if LV_USE_FONT_PLACEHOLDER
    dsc_out->box_w = font_p->line_height / 2;
    dsc_out->adv_w = dsc_out->box_w + 2;
#else
    dsc_out->box_w = 0;
    dsc_out->adv_w = no_room(letter) ? 0 : font_p->line_height / 6;
#endif

    dsc_out->resolved_font = NULL;
    dsc_out->box_h = font_p->line_height;
    dsc_out->ofs_x = 0;
    dsc_out->ofs_y = 0;
    dsc_out->format = LV_FONT_GLYPH_FORMAT_A1;
    dsc_out->is_placeholder = true;

    return false;
}

uint16_t lv_font_get_glyph_width(const lv_font_t * font, uint32_t letter, uint32_t letter_next)
{
    LV_ASSERT_NULL(font);
    lv_font_glyph_dsc_t g;

    /*Return zero if letter is marker*/
    if(lv_text_is_marker(letter)) return 0;

    lv_font_get_glyph_dsc(font, &g, letter, letter_next);
    return g.adv_w;
}

void lv_font_set_kerning(lv_font_t * font, lv_font_kerning_t kerning)
{
    LV_ASSERT_NULL(font);
    font->kerning = kerning;
}

int32_t lv_font_get_line_height(const lv_font_t * font)
{
    return font->line_height;
}

const lv_font_t * lv_font_default(void)
{
    return LV_FONT_DEFAULT;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
