#ifndef FRONTEND_H
#define FRONTEND_H

#include <stdbool.h>
#include "settings.h"   /* MenuAction -- the front end speaks the same language
                         * as the pause menu, so main.c keeps one switch shape */

/*
 * The front end: the intro card that plays once at startup, and the main menu
 * that the game lands on instead of dropping you straight into the arena.
 *
 * Both draw over the LIVE world -- main.c keeps ticking the simulation with an
 * empty input and slowly orbits the camera, so the arena behind the menu is the
 * real thing rather than a still image. Neither of them touches simulation
 * state; they only read the clock, exactly like the rest of the presentation
 * layer.
 */

/* Presentation identity. The window title, the intro card and the menu all
 * agree on these. The caps variant is spelled out rather than upper-cased at
 * runtime because it is a title treatment, not a transformation of a string. */
#define GAME_TITLE      "Arena Fighter"
#define GAME_TITLE_CAPS "ARENA FIGHTER"
#define GAME_VERSION    "v0.1-dev"

/* ------------------------------- intro ----------------------------------- */

void intro_reset(void);

/*
 * Advance and draw one frame of the intro. `dt` is wall-clock seconds.
 * Returns true on the frame the sequence has finished.
 *
 * Any key or click skips it. Draw the main menu UNDERNEATH this: the card's
 * final beat is a black scrim lifting off, so whatever is behind it is what the
 * intro dissolves into.
 */
bool intro_draw(float dt);

/* ----------------------------- main menu --------------------------------- */

/*
 * The landing screen. Same immediate-mode contract as the settings pages: it
 * draws itself and reports what the user asked for this frame -- one of
 * MENU_PLAY, MENU_OPEN_SETTINGS, MENU_OPEN_KEYBINDS or MENU_QUIT.
 */
MenuAction main_menu(void);

#endif /* FRONTEND_H */
