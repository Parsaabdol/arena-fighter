#include "hero.h"

#include <string.h>

/* The same family the UI is built from, so a fighter never clashes with the
 * menu he is standing behind. */
static const Color COLORS[] = {
    {  94, 129, 172, 255 },   /* Steel   -- the original body colour  */
    { 235, 203, 139, 255 },   /* Sand    -- the original accent       */
    { 191,  97, 106, 255 },   /* Ember                                */
    { 163, 190, 140, 255 },   /* Moss                                 */
    { 180, 142, 173, 255 },   /* Orchid                               */
    { 136, 192, 208, 255 },   /* Frost                                */
    { 229, 233, 240, 255 },   /* Bone                                 */
    {  59,  66,  82, 255 },   /* Coal                                 */
};

static const char *const COLOR_NAMES[] = {
    "Steel", "Sand", "Ember", "Moss", "Orchid", "Frost", "Bone", "Coal",
};

#define COLOR_COUNT ((int)(sizeof(COLORS) / sizeof(COLORS[0])))

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void hero_default(Hero *h)
{
    h->model[0] = '\0';          /* the built-in blocks */
    h->body_index = 0;           /* Steel  */
    h->accent_index = 1;         /* Sand   */
}

bool hero_equal(const Hero *a, const Hero *b)
{
    return a->body_index   == b->body_index
        && a->accent_index == b->accent_index
        && strncmp(a->model, b->model, HERO_NAME_MAX) == 0;
}

int   hero_color_count(void) { return COLOR_COUNT; }
Color hero_color(int index)  { return COLORS[clampi(index, 0, COLOR_COUNT - 1)]; }

const char *hero_color_name(int index)
{
    return COLOR_NAMES[clampi(index, 0, COLOR_COUNT - 1)];
}

const char *const *hero_color_names(void) { return COLOR_NAMES; }

Color hero_body(const Hero *h)   { return hero_color(h->body_index); }
Color hero_accent(const Hero *h) { return hero_color(h->accent_index); }
