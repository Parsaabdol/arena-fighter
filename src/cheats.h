#ifndef CHEATS_H
#define CHEATS_H

#include <stdbool.h>
#include "game.h"      /* Cheats lives in game.h -- the simulation reads it */

void cheats_default(Cheats *c);

/* Draw and run the cheat overlay for one frame. Toggles take effect
 * immediately (no Apply step -- that is the point of a cheat menu).
 * Returns true when the user asked to close it. */
bool cheats_menu(Cheats *c);

#endif /* CHEATS_H */
