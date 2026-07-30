/*
 * SkinPreview.exe -- look at an imported skin without starting the game.
 *
 *     SkinPreview.exe <file.glb> [--state walk|sprint|crouch|attack|jump]
 *
 * This is NOT a second renderer. It links the same `model.c` and `anim_gltf.c`
 * the game does, so everything that decides how a skin looks -- the bind-pose
 * rewrite, the up-axis vote, the fit that measures the idle pose and ignores
 * carried props, the alias tables that resolve a clip per state -- is the same
 * code producing the same answer. What it leaves out is the game: no arena, no
 * simulation, no settings file. It builds a `Fighter` by hand, hands it to
 * `model_animate`, and draws it.
 *
 * That division is the point. A preview that shared nothing with the game would
 * drift until it lied; a preview that IS the game means launching the game.
 * This is the third option -- the game's own loader, on a turntable.
 */
#include "game.h"
#include "model.h"
#include "raylib.h"
#include "raymath.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define WIN_W 900
#define WIN_H 700

#define ORBIT_RATE   0.35f    /* radians/sec of idle turntable drift        */
#define PITCH_MIN    0.05f
#define PITCH_MAX    1.30f
#define DIST_MIN     1.6f
#define DIST_MAX    12.0f

/* The states worth looking at, and the walking speed each implies. Speed
 * matters because movement clips are paced by ground speed, not by a clock --
 * standing still would freeze a walk cycle mid-stride. */
static const struct {
    const char *name;
    AnimState   state;
    float       speed;
} VIEWS[] = {
    { "idle",   ANIM_IDLE,   0.0f      },
    { "walk",   ANIM_WALK,   MOVE_MAX * 0.55f },
    { "sprint", ANIM_SPRINT, MOVE_MAX * SPRINT_MULT },
    { "crouch", ANIM_CROUCH, MOVE_MAX * 0.30f },
    { "attack", ANIM_ATTACK, 0.0f      },
    { "jump",   ANIM_JUMP_APEX, 0.0f   },
};
#define VIEW_COUNT ((int)(sizeof(VIEWS) / sizeof(VIEWS[0])))

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: SkinPreview.exe <file.glb> [--state idle|walk|sprint|"
               "crouch|attack|jump]\n");
        return 2;
    }

    int view = 0;
    for (int i = 2; i < argc - 1; i++)
        if (strcmp(argv[i], "--state") == 0)
            for (int v = 0; v < VIEW_COUNT; v++)
                if (strcmp(argv[i + 1], VIEWS[v].name) == 0) view = v;

    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(WIN_W, WIN_H, "Skin preview");
    SetTargetFPS(60);

    /* model.c resolves names against assets/ beside the executable, which is
     * exactly where the importer puts them, so only the file name travels. */
    model_scan();
    const char *file = GetFileName(argv[1]);
    bool ok = model_active(file);

    Camera3D cam = {
        .position   = { 0.0f, 1.4f, 4.2f },
        .target     = { 0.0f, 0.95f, 0.0f },
        .up         = { 0.0f, 1.0f, 0.0f },
        .fovy       = 45.0f,
        .projection = CAMERA_PERSPECTIVE,
    };

    float yaw = 0.0f, pitch = 0.35f, dist = 4.2f;
    bool  spin = true;

    /* One Fighter, posed by hand. The simulation is not running -- there is
     * nothing to simulate -- so the fields the renderer reads are set directly
     * and the clip playhead follows from them. */
    Fighter f;
    memset(&f, 0, sizeof f);
    f.grounded = true;
    f.health   = 100.0f;

    while (!WindowShouldClose()) {
        float dt = GetFrameTime();

        for (int v = 0; v < VIEW_COUNT; v++)
            if (IsKeyPressed(KEY_ONE + v)) view = v;
        if (IsKeyPressed(KEY_SPACE)) spin = !spin;

        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            Vector2 d = GetMouseDelta();
            yaw   -= d.x * 0.006f;
            pitch += d.y * 0.006f;
            spin = false;
        }
        if (spin) yaw += ORBIT_RATE * dt;
        pitch = Clamp(pitch, PITCH_MIN, PITCH_MAX);
        dist  = Clamp(dist - GetMouseWheelMove() * 0.4f, DIST_MIN, DIST_MAX);

        /* Drive the one field each state actually needs. Everything else the
         * clip pacing reads -- ground speed, air time, the attack playhead --
         * comes from these. */
        f.anim       = VIEWS[view].state;
        f.vx         = VIEWS[view].speed;
        f.crouching  = (VIEWS[view].state == ANIM_CROUCH);
        f.crouch_blend = f.crouching ? 1.0f : 0.0f;
        f.grounded   = (VIEWS[view].state != ANIM_JUMP_APEX);
        f.anim_time += dt;

        if (VIEWS[view].state == ANIM_JUMP_APEX) {
            f.air_time += dt;
            if (f.air_time > 2.0f * JUMP_SPEED / GRAVITY) f.air_time = 0.0f;
        } else {
            f.air_time = 0.0f;
        }

        if (VIEWS[view].state == ANIM_ATTACK) {
            /* Loop the swing so the strike keeps coming round. */
            f.attacking    = true;
            f.attack_index = 0;
            f.attack_time += dt;
            if (f.attack_time > 1.2f) f.attack_time = 0.0f;
        } else {
            f.attacking   = false;
            f.attack_time = 0.0f;
        }

        model_animate(&f, dt);

        cam.position = (Vector3){ sinf(yaw) * cosf(pitch) * dist,
                                  0.95f + sinf(pitch) * dist,
                                  cosf(yaw) * cosf(pitch) * dist };
        cam.target = (Vector3){ 0.0f, 0.95f, 0.0f };

        BeginDrawing();
            ClearBackground((Color){ 28, 30, 34, 255 });
            BeginMode3D(cam);
                DrawGrid(12, 0.5f);
                if (ok) model_draw(&f, WHITE);
            EndMode3D();

            if (!ok) {
                DrawText("could not load that file", 20, 20, 20, RED);
                DrawText(file, 20, 46, 16, GRAY);
            } else {
                DrawText(TextFormat("%s   %d clips, %d/%d states matched",
                                    file, model_clip_count(),
                                    model_matched_count(), ANIM_COUNT),
                         20, 16, 16, RAYWHITE);
                DrawText(TextFormat("%s  ->  %s", VIEWS[view].name,
                                    model_state_clip(VIEWS[view].state)),
                         20, 40, 16, (Color){ 150, 200, 255, 255 });
            }
            DrawText("1-6 state    drag to turn    wheel to zoom    "
                     "space to spin", 20, GetScreenHeight() - 28, 15,
                     (Color){ 140, 140, 150, 255 });
        EndDrawing();
    }

    model_unload();
    CloseWindow();
    return ok ? 0 : 1;
}
