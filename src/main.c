#include "game.h"
#include "settings.h"
#include "cheats.h"

#include "raylib.h"

#include <math.h>

#ifdef DIAG
#include <stdio.h>
#endif

/* Raw key directions, still in camera space: -z is "forward on screen". */
static Input sample_input(void)
{
    Input in = (Input){ 0 };

    if (IsKeyDown(KEY_W)) in.move_z -= 1.0f;
    if (IsKeyDown(KEY_S)) in.move_z += 1.0f;
    if (IsKeyDown(KEY_A)) in.move_x -= 1.0f;
    if (IsKeyDown(KEY_D)) in.move_x += 1.0f;

    in.sprint  = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
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

/* Which menu page is up. Escape opens the pause menu; the option pages are one
 * step further in, so unpausing never puts you in front of a wall of sliders. */
typedef enum Screen {
    SCREEN_NONE = 0,
    SCREEN_PAUSE,
    SCREEN_SETTINGS,
    SCREEN_KEYBINDS
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
    InitWindow(1280, 720, "Arena Fighter");

    /* Escape belongs to the menus, so it must not also close the window. The
     * only ways out are the Quit button and the title bar X. */
    SetExitKey(KEY_NULL);

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
    Screen screen = SCREEN_NONE;
    bool  cheats_open = false;
    bool  quit = false;
    bool  look_warmup = true;   /* swallow the first mouse delta after locking */
    bool  jump_latched = false;
    bool  attack_latched = false;

    while (!WindowShouldClose() && !quit) {

        /* While the options menu is waiting for a key to bind, every keypress
         * belongs to it -- including Escape, which cancels the capture. */
        bool rebinding = settings_is_rebinding();

        if (!rebinding) {
            if (IsKeyPressed(KEY_ESCAPE)) {
                if (cheats_open) {
                    cheats_open = false;
                } else switch (screen) {
                case SCREEN_NONE:
                    pending = applied;              /* start clean each time */
                    screen = SCREEN_PAUSE;
                    break;
                case SCREEN_PAUSE:
                    screen = SCREEN_NONE;
                    break;
                default:                            /* an option page */
                    commit_settings(&applied, &pending);
                    screen = SCREEN_PAUSE;
                    break;
                }
            }
            /* The cheat key is rebindable, so it is read from settings rather
             * than hardcoded. */
            if (screen == SCREEN_NONE && IsKeyPressed(applied.cheat_key))
                cheats_open = !cheats_open;
        }

        bool paused = (screen != SCREEN_NONE) || cheats_open;

        /* ---- cursor + look ---------------------------------------------- */
        if (paused) {
            if (IsCursorHidden()) EnableCursor();
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
        if (paused) {
            /* Freeze the world. Collapsing prev onto curr means the renderer
             * has nothing to interpolate, so the paused frame is rock steady
             * instead of jittering between two stale states. */
            prev = curr;
            accumulator = 0.0f;
        } else {
            float frame = GetFrameTime();
            if (frame > MAX_FRAME_TIME) frame = MAX_FRAME_TIME; /* no death spiral */
            accumulator += frame;

            Input in = sample_input();
            input_to_world(&in, render_camera_yaw());
            in.jump   = jump_latched;
            in.attack = attack_latched;

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
        float alpha = paused ? 0.0f : (accumulator / TICK_DT);

        /* ---- draw -------------------------------------------------------- */
        HudInfo hud = {
            .fps_limit = settings_fps_limit(&applied),
            .vsync     = applied.vsync,
            .cheat_key = settings_key_name(applied.cheat_key),
        };

        render_begin();
            render_world(&prev, &curr, alpha);
            render_hud(&curr, alpha, &hud, &cheats);

            switch (screen) {
            case SCREEN_PAUSE:
                switch (pause_menu()) {
                case MENU_RESUME: screen = SCREEN_NONE;     break;
                case MENU_QUIT:   quit   = true;            break;
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
            case SCREEN_KEYBINDS: {
                MenuAction a = (screen == SCREEN_SETTINGS)
                             ? settings_menu(&pending, &applied)
                             : keybinds_menu(&pending, &applied);
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
                case MENU_BACK:
                    commit_settings(&applied, &pending);
                    screen = SCREEN_PAUSE;
                    break;
                default: break;
                }
                break;
            }

            case SCREEN_NONE:
            default:
                break;
            }

            if (screen == SCREEN_NONE && cheats_open) {
                if (cheats_menu(&cheats)) cheats_open = false;
            }
        render_end();
    }

    CloseWindow();
    return 0;
}
