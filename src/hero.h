#ifndef HERO_H
#define HERO_H

#include "raylib.h"

/*
 * Who you are playing as: which skin, in which colours, at what size.
 *
 * Plain old data and copyable with `=`, like everything else that gets saved --
 * it lives inside `Settings` so it inherits the whole pending/applied/Apply
 * flow and the one versioned save file, rather than growing a second one.
 *
 * The skin is stored by FILE NAME rather than by an index into the scanned
 * list, because that list is whatever happens to be sitting in `assets/` on
 * this machine. An index would silently mean a different character the moment
 * you added a file; a name either resolves or falls back to the built-in.
 */

#define HERO_NAME_MAX 32

/*
 * There is deliberately no size option. Every fighter stands FIGHTER_HEIGHT
 * tall, imported skins included -- see game.h. Letting appearance change how
 * big you are would change how far you reach and how big a target you make,
 * which is a stat wearing a costume's clothes.
 */
typedef struct Hero {
    char model[HERO_NAME_MAX];  /* file in assets/, or "" for the built-in */
    int  body_index;            /* index into the colour table             */
    int  accent_index;
} Hero;

void hero_default(Hero *h);
bool hero_equal(const Hero *a, const Hero *b);

/* Colours and sizes are tables so the menu can cycle them and the save file
 * can store an index instead of a raw value. */
int          hero_color_count(void);
Color        hero_color(int index);
const char  *hero_color_name(int index);
const char *const *hero_color_names(void);

/* Convenience for the renderer. */
Color hero_body(const Hero *h);
Color hero_accent(const Hero *h);

#endif /* HERO_H */
