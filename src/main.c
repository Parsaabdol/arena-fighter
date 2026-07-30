#include "game.h"
#include "settings.h"
#include "cheats.h"
#include "frontend.h"
#include "model.h"

#include "raylib.h"

#include <math.h>

#ifdef DIAG
#include <stdio.h>
#endif

/* Raw key directions, still in camera space: -z is "forward on screen". */
static Input sample_input(const Settings *s)
{
    Input in = (Input){ 0 };

    if (IsKeyDown(KEY_W)) in.move_z -= 1.0f;
    if (IsKeyDown(KEY_S)) in.move_z += 1.0f;
    if (IsKeyDown(KEY_A)) in.move_x -= 1.0f;
    if (IsKeyDown(KEY_D)) in.move_x += 1.0f;

    in.sprint  = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    in.crouch  = IsKeyDown(s->crouch_key);
    in.special = IsMouseButtonDown(MOUSE_BUTTON_RIGHT) || IsKeyDown(KEY_K);
    /* in.attack is edge-triggered and latched in the main loop, like jump. */

    return in;
}

/* Rotate the raw input into world space using the camera's yaw, so "forward"
 * always means "away from the camera" no matter where you are looking. Without
 * this, mouselook would make WASD feel broken the moment you turned. */
static void input_to_world(Input *in, float yaw)
{
    float s = sinf(yaw), c = cosf(yaw);
    float x = in->move_x, z = in->move_z;
    in->move_x =  x * c + z * s;
    in->move_z = -x * s + z * c;
}

/* Where the program is, at the top level. All three states share one window and
 * one world: the front end keeps the simulation ticking with an empty input so
 * the arena behind the menu is the live thing, and only what is drawn over it
 * and who owns the mouse actually change. */
typedef enum AppState {
    APP_INTRO = 0,      /* the title card, once, at startup   */
    APP_MENU,           /* the landing screen                 */
    APP_GAME            /* playing                            */
} AppState;

/* Which menu page is up. Escape opens the pause menu; the option pages are one
 * step further in, so unpausing never puts you in front of a wall of sliders.
 * The same pages serve the main menu, where SCREEN_NONE means the title screen
 * itself rather than "no menu at all". */
typedef enum Screen {
    SCREEN_NONE = 0,
    SCREEN_PAUSE,
    SCREEN_SETTINGS,
    SCREEN_KEYBINDS,
    SCREEN_CUSTOMIZE,
    SCREEN_SKINS        /* one level below customize */
} Screen;

/* Commit whatever the menus edited: push it into the window and write it to
 * disk. Called on Apply and on every exit from an option page, so a change is
 * never silently lost just because you left by a different door. */
static void commit_settings(Settings *applied, const Settings *pending)
{
    if (settings_equal(applied, pending)) return;
    *applied = *pending;
    settings_apply_all(applied);
    settings_save(applied);
}

int main(void)
{
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(1280, 720, GAME_TITLE);

    /* Escape belongs to the menus, so it must not also close the window. The
     * only ways out are the Quit button and the title bar X. */
    SetExitKey(KEY_NULL);

    /* Find any skins sitting in assets/ before the save file is read, so a
     * remembered skin can be matched against what is actually there. */
    model_scan();

    /* `applied` is what the game is actually running with. `pending` is what
     * the menu is editing. They only converge when you press Apply. */
    Settings applied;
    settings_default(&applied);
    bool loaded = settings_load(&applied);  /* silently keeps defaults if absent */
    settings_apply_all(&applied);
    Settings pending = applied;

    /* Cheats are deliberately session-only: they are never written to disk, so
     * they can't silently persist into a later run. */
    Cheats cheats;
    cheats_default(&cheats);

#ifdef DIAG
    printf("DIAG loaded=%d fps_idx=%d res_idx=%d disp=%d vsync=%d key=%d -> screen %dx%d\n",
           (int)loaded, applied.fps_index, applied.res_index,
           applied.display_mode, (int)applied.vsync, applied.cheat_key,
           GetScreenWidth(), GetScreenHeight());
    fflush(stdout);
#endif
    (void)loaded;

    render_init();

    World prev, curr;
    world_init(&curr);
    prev = curr;

    float accumulator = 0.0f;
    AppState app = APP_INTRO;
    Screen screen = SCREEN_NONE;
    bool  cheats_open = false;
    bool  quit = false;
    bool  look_warmup = true;   /* swallow the first mouse delta after locking */
    bool  jump_latched = false;
    bool  attack_latched = false;

    intro_reset();

    while (!WindowShouldClose() && !quit) {

        /* Wall-clock seconds for this frame, clamped once here. A stall must
         * not send the simulation into an unbounded catch-up loop, and it must
         * not skip a beat of the intro either. This is the only place the real
         * clock is read on the way to the simulation. */
        float dt = GetFrameTime();
        if (dt > MAX_FRAME_TIME) dt = MAX_FRAME_TIME;

        /* While the options menu is waiting for a key to bind, every keypress
         * belongs to it -- including Escape, which cancels the capture. During
         * the intro every keypress is a skip, which the intro reads itself. */
        bool rebinding = settings_is_rebinding();

        if (!rebinding && app != APP_INTRO) {
            if (IsKeyPressed(KEY_ESCAPE)) {
                if (cheats_open) {
                    cheats_open = false;
                } else switch (screen) {
                case SCREEN_NONE:
                    /* Nothing to pause in the main menu -- it is already the
                     * outermost screen there. */
                    if (app == APP_GAME) {
                        pending = applied;          /* start clean each time */
                        screen = SCREEN_PAUSE;
                    }
                    break;
                case SCREEN_PAUSE:
                    screen = SCREEN_NONE;
                    break;
                case SCREEN_SKINS:
                    /* One level down, so Escape steps back one level. */
                    screen = SCREEN_CUSTOMIZE;
                    break;
                default:                            /* an option page */
                    commit_settings(&applied, &pending);
                    screen = (app == APP_GAME) ? SCREEN_PAUSE : SCREEN_NONE;
                    break;
                }
            }
            /* The cheat key is rebindable, so it is read from settings rather
             * than hardcoded. It is a gameplay tool, so the front end ignores
             * it -- there is nothing to cheat at on the title screen. */
            if (app == APP_GAME && screen == SCREEN_NONE &&
                IsKeyPressed(applied.cheat_key))
                cheats_open = !cheats_open;
        }

        /* Playing means: in the game, no menu page up, no cheat panel. */
        bool playing = (app == APP_GAME && screen == SCREEN_NONE && !cheats_open);

        /* The world keeps ticking in the front end so the fighter breathes and
         * the camera drifts behind the menu; it just gets no input. Only a
         * paused GAME freezes. */
        bool simulating = playing || (app != APP_GAME);

        /* ---- cursor + look ---------------------------------------------- */
        if (!playing) {
            if (IsCursorHidden()) EnableCursor();

            /* Drop any latched press on the way out, so a jump pressed a
             * moment before opening a menu does not fire on the way back in --
             * or worse, survive into the next run. */
            jump_latched = attack_latched = false;
        } else {
            if (!IsCursorHidden()) {
                DisableCursor();     /* hides AND locks to the window centre */
                look_warmup = true;
            }
            Vector2 md = GetMouseDelta();
            if (look_warmup) look_warmup = false;   /* first delta is garbage */
            else render_camera_look(md.x, md.y, settings_sensitivity(&applied));

            /* Latch the jump and attack presses. At 120 Hz sim and higher
             * render rates, some frames produce zero ticks -- a press sampled
             * on one of those frames would otherwise be silently dropped. */
            if (IsKeyPressed(KEY_SPACE)) jump_latched = true;
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsKeyPressed(KEY_J))
                attack_latched = true;
        }

        /* ---- simulate ---------------------------------------------------- */
        if (!simulating) {
            /* Freeze the world. Collapsing prev onto curr means the renderer
             * has nothing to interpolate, so the paused frame is rock steady
             * instead of jittering between two stale states. */
            prev = curr;
            accumulator = 0.0f;
        } else {
            accumulator += dt;

            Input in = (Input){ 0 };
            if (playing) {
                in = sample_input(&applied);
                input_to_world(&in, render_camera_yaw());
                in.jump   = jump_latched;
                in.attack = attack_latched;
            } else {
                /* The appearance pages hand the camera to the player, so the
                 * backdrop stops drifting while either is open. */
                bool inspecting = (screen == SCREEN_CUSTOMIZE ||
                                   screen == SCREEN_SKINS);
                render_camera_idle_orbit(dt, !inspecting);
            }

            while (accumulator >= TICK_DT) {
                prev = curr;          /* previous state to interpolate from */
                world_tick(&curr, in, &cheats);
                /* One action per press, not per tick. */
                in.jump = false;      jump_latched = false;
                in.attack = false;    attack_latched = false;
                accumulator -= TICK_DT;
            }
        }

        /* How far between `prev` and `curr` this frame lands. */
        float alpha = simulating ? (accumulator / TICK_DT) : 0.0f;

        /* ---- draw -------------------------------------------------------- */
        HudInfo hud = {
            .fps_limit  = settings_fps_limit(&applied),
            .vsync      = applied.vsync,
            .cheat_key  = settings_key_name(applied.cheat_key),
            .crouch_key = settings_key_name(applied.crouch_key),
        };

        /* While an appearance page is open the world is drawn with the hero
         * being EDITED rather than the one in force, which is what makes the
         * fighter standing in the arena behind the panel a live preview. */
        const Hero *shown = (screen == SCREEN_CUSTOMIZE || screen == SCREEN_SKINS)
                          ? &pending.hero : &applied.hero;

        render_begin();
            render_world(&prev, &curr, alpha, shown);

            /* The HUD belongs to the game, not to the title screen. */
            if (app == APP_GAME) render_hud(&curr, alpha, &hud, &cheats);

            if (app == APP_INTRO) {
                /* The menu is drawn UNDERNEATH the card, so the card lifting
                 * away IS the menu arriving -- one continuous shot instead of a
                 * cut. Its action is discarded until the intro is really over,
                 * which is also what makes the release of a skip-click safe. */
                main_menu();
                if (intro_draw(dt)) app = APP_MENU;
            } else switch (screen) {
            case SCREEN_NONE:
                if (app == APP_MENU) {
                    switch (main_menu()) {
                    case MENU_PLAY:
                        /* A fresh run every time, from a known state. */
                        world_init(&curr);
                        prev = curr;
                        accumulator = 0.0f;
                        app = APP_GAME;
                        break;
                    case MENU_QUIT: quit = true; break;
                    case MENU_OPEN_CUSTOMIZE:
                        pending = applied;
                        screen = SCREEN_CUSTOMIZE;
                        break;
                    case MENU_OPEN_SETTINGS:
                        pending = applied;
                        screen = SCREEN_SETTINGS;
                        break;
                    case MENU_OPEN_KEYBINDS:
                        pending = applied;
                        screen = SCREEN_KEYBINDS;
                        break;
                    default: break;
                    }
                } else if (cheats_open) {
                    if (cheats_menu(&cheats)) cheats_open = false;
                }
                break;

            case SCREEN_PAUSE:
                switch (pause_menu()) {
                case MENU_RESUME: screen = SCREEN_NONE;     break;
                case MENU_QUIT:   quit   = true;            break;
                case MENU_MAIN_MENU:
                    app = APP_MENU;
                    cheats_open = false;
                    screen = SCREEN_NONE;
                    break;
                case MENU_OPEN_SETTINGS:
                    pending = applied;
                    screen = SCREEN_SETTINGS;
                    break;
                case MENU_OPEN_KEYBINDS:
                    pending = applied;
                    screen = SCREEN_KEYBINDS;
                    break;
                default: break;
                }
                break;

            case SCREEN_SETTINGS:
            case SCREEN_KEYBINDS:
            case SCREEN_CUSTOMIZE:
            case SCREEN_SKINS: {
                MenuAction a = (screen == SCREEN_SETTINGS)
                             ? settings_menu(&pending, &applied)
                             : (screen == SCREEN_KEYBINDS)
                             ? keybinds_menu(&pending, &applied)
                             : (screen == SCREEN_CUSTOMIZE)
                             ? customize_menu(&pending.hero, &applied.hero)
                             : modskins_menu(&pending.hero, &applied.hero);
                switch (a) {
                case MENU_APPLY:
                    commit_settings(&applied, &pending);
                    break;
                case MENU_DEFAULTS:
                    settings_default(&pending);
                    break;
                case MENU_REVERT:
                    pending = applied;
                    break;
                case MENU_OPEN_SKINS:
                    /* Deeper, not sideways: the mod list keeps the edits the
                     * page above it has already made. */
                    screen = SCREEN_SKINS;
                    /* Re-read assets/ on the way in. Skins arrive while the
                     * game is running -- tools/export_hero.py writes one in
                     * about a minute -- and having to restart to see it is a
                     * poor answer when opening the page can just look again.
                     * Cheap: a directory listing, once, on a click. */
                    model_scan();
                    break;
                case MENU_BACK:
                    if (screen == SCREEN_SKINS) {
                        screen = SCREEN_CUSTOMIZE;   /* back one level */
                    } else {
                        commit_settings(&applied, &pending);
                        screen = (app == APP_GAME) ? SCREEN_PAUSE : SCREEN_NONE;
                    }
                    break;
                default: break;
                }
                break;
            }

            default:
                break;
            }
        render_end();
    }

    model_unload();     /* before the GL context goes away */
    CloseWindow();
    return 0;
}
