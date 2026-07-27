#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>

typedef enum DisplayMode {
    DISPLAY_WINDOWED = 0,
    DISPLAY_BORDERLESS,
    DISPLAY_FULLSCREEN,
    DISPLAY_MODE_COUNT
} DisplayMode;

typedef struct Settings {
    int  fps_index;      /* index into the FPS options table         */
    int  res_index;      /* index into the resolution table          */
    int  display_mode;   /* DisplayMode                              */
    int  sens_index;     /* index into the mouse sensitivity table   */
    int  cheat_key;      /* raylib keycode that opens the cheat menu */
    bool vsync;
} Settings;

/* What the menu wants the caller to do this frame. Shared with the front end
 * (see frontend.h) so main.c routes every menu through one switch shape. */
typedef enum MenuAction {
    MENU_NONE = 0,
    MENU_APPLY,          /* commit `pending` and write it to disk */
    MENU_REVERT,         /* throw `pending` away                  */
    MENU_DEFAULTS,       /* load the factory defaults into `pending` */
    MENU_BACK,           /* leave this page, back to wherever we came from */
    MENU_OPEN_SETTINGS,
    MENU_OPEN_KEYBINDS,
    MENU_RESUME,
    MENU_PLAY,           /* main menu: start a fresh run           */
    MENU_MAIN_MENU,      /* pause menu: abandon the run, back to the title */
    MENU_QUIT
} MenuAction;

void settings_default(Settings *s);

/* Push settings into the window/GPU. */
void settings_apply_all(const Settings *s);

/* Accessors used by the game loop and HUD. */
int   settings_fps_limit(const Settings *s);
float settings_sensitivity(const Settings *s);

bool  settings_equal(const Settings *a, const Settings *b);

/* Persistence. The file sits next to the executable and is versioned and
 * checksummed, so a stale or corrupt one is rejected rather than trusted. */
bool  settings_save(const Settings *s);
bool  settings_load(Settings *s);

/* Human-readable name for a raylib keycode, e.g. "Tab", "F5", "K". */
const char *settings_key_name(int key);

/* True while the menu is waiting for the user to press a key to rebind. The
 * caller must not act on Escape during that window -- it cancels the capture
 * instead of closing the menu. */
bool settings_is_rebinding(void);

/*
 * The three menu pages. Escape opens the pause menu, which is just a list of
 * destinations -- options and keybinds each live on their own page so neither
 * is in the way when you only wanted to unpause.
 *
 * Each returns the action the caller should take this frame. `pending` is the
 * copy being edited; `applied` is what the game is actually running with, and
 * is only read to tell whether anything is dirty.
 */
MenuAction pause_menu(void);
MenuAction settings_menu(Settings *pending, const Settings *applied);
MenuAction keybinds_menu(Settings *pending, const Settings *applied);

#endif /* SETTINGS_H */
