#include "game.h"

#include "raylib.h"
#include "rlgl.h"

#include <math.h>

#define PI_F 3.14159265358979323846f
#define RAD2DEGF (180.0f / PI_F)

/* The camera lives here rather than in the world because it is a presentation
 * concern -- it must never affect the simulation. */
static Camera3D g_cam;
static Vector3  g_cam_look;      /* smoothed point the camera aims at */

#define PITCH_DEFAULT 0.42f      /* radians above the horizon ~24 deg     */
#define PITCH_MIN 0.06f          /* just above ground level ~3.5 deg      */
#define PITCH_MAX 1.30f          /* near straight down     ~74 deg        */
#define CAM_DIST  9.5f

/* Orbit angles, driven by the mouse. */
static float g_yaw   = 0.0f;     /* radians; 0 puts the camera on +z      */
static float g_pitch = PITCH_DEFAULT;

/* Front-end drift: one lap every ~60 seconds, slow enough that it reads as a
 * living shot rather than as a turntable. */
#define IDLE_ORBIT_RATE 0.105f   /* radians/second                        */

/* The menu's rows run down the middle of the screen, so the front end aims the
 * camera to one side of the fighter to keep him out from behind them. The
 * offset is applied in CAMERA space, which is what keeps him parked on the same
 * side of the frame while the orbit turns all the way round. */
#define IDLE_LOOK_SIDE  2.8f     /* world units                           */

static float g_look_side;        /* current offset, eased                 */
static float g_look_side_want;   /* re-asserted every front-end frame     */

/* ------------------------------------------------------------------------ */

void render_init(void)
{
    g_cam_look = (Vector3){ 0.0f, 1.2f, 0.0f };

    g_cam.position   = (Vector3){ 0.0f, 5.0f, 9.5f };
    g_cam.target     = g_cam_look;
    g_cam.up         = (Vector3){ 0.0f, 1.0f, 0.0f };
    g_cam.fovy       = 55.0f;
    g_cam.projection = CAMERA_PERSPECTIVE;
}

void render_camera_look(float dx, float dy, float sensitivity)
{
    /* Mouse right turns the view right, which means orbiting the camera the
     * other way round the character -- hence the minus. Mouse down looks down,
     * which raises the camera, so pitch increases. */
    g_yaw   -= dx * sensitivity;
    g_pitch += dy * sensitivity;

    if (g_pitch < PITCH_MIN) g_pitch = PITCH_MIN;
    if (g_pitch > PITCH_MAX) g_pitch = PITCH_MAX;

    /* Keep yaw bounded so it cannot drift into float-precision mush after a
     * long session of spinning in one direction. */
    if (g_yaw >  PI_F) g_yaw -= 2.0f * PI_F;
    if (g_yaw < -PI_F) g_yaw += 2.0f * PI_F;
}

float render_camera_yaw(void)
{
    return g_yaw;
}

/* Frame-rate independent exponential smoothing.
 * A plain `a += (b - a) * 0.1f` moves faster at higher FPS -- the camera would
 * literally behave differently on a better monitor. This does not. */
static float smooth(float current, float target, float rate, float dt)
{
    float t = 1.0f - expf(-rate * dt);
    return current + (target - current) * t;
}

/* The front end has no mouselook, so the camera drifts on its own to keep the
 * arena behind the menu alive. Pitch eases back to the gameplay default at the
 * same time, which means returning from a run that ended with the camera
 * pointing at the floor tidies itself up -- and pressing Play needs no reset,
 * because the camera is already where gameplay wants it. */
void render_camera_idle_orbit(float dt)
{
    g_yaw += IDLE_ORBIT_RATE * dt;
    if (g_yaw > PI_F) g_yaw -= 2.0f * PI_F;

    g_pitch = smooth(g_pitch, PITCH_DEFAULT, 2.5f, dt);
    g_look_side_want = IDLE_LOOK_SIDE;
}

/* --------------------------- character model ----------------------------- */
/*
 * The fighter is a hierarchy of boxes. Each limb is drawn by moving to its
 * pivot (shoulder / hip), rotating there, then drawing a box that hangs down
 * from that pivot. Composing transforms by hand like this is the whole reason
 * to build the model in code: it is 3D math you can actually see.
 */

#define LEG_LEN    0.80f
#define LEG_THICK  0.22f
#define TORSO_H    0.70f
#define TORSO_W    0.52f
#define TORSO_D    0.30f
#define ARM_LEN    0.68f
#define ARM_THICK  0.18f
#define HEAD_SIZE  0.40f

#define HIP_Y      LEG_LEN
#define SHOULDER_Y (LEG_LEN + TORSO_H - 0.06f)
#define HEAD_Y     (LEG_LEN + TORSO_H + HEAD_SIZE * 0.5f)

static void draw_limb(float px, float py, float pz,
                      float pitch_deg, float len, float thick, Color fill)
{
    rlPushMatrix();
        rlTranslatef(px, py, pz);
        rlRotatef(pitch_deg, 1.0f, 0.0f, 0.0f);
        Vector3 c = (Vector3){ 0.0f, -len * 0.5f, 0.0f };
        DrawCube(c, thick, len, thick, fill);
        DrawCubeWires(c, thick, len, thick, Fade(BLACK, 0.35f));
    rlPopMatrix();
}

static void draw_fighter(const Fighter *f, Color body, Color accent)
{
    float speed = sqrtf(f->vx * f->vx + f->vz * f->vz);

    /* How "walky" the pose is: 0 when standing, 1 at full stride. */
    float walk = fminf(speed / 5.0f, 1.0f);

    /* Sprint reshapes the same walk cycle rather than replacing it: a longer
     * stride, a deeper bob, more forward lean, and arms driven up into a pump.
     * The stride FREQUENCY needs no special case -- gait advances by distance
     * travelled, so moving faster already steps faster. */
    float sb = f->sprint_blend;                             /* 0..1, eased */

    float swing   = sinf(f->gait) * (42.0f + 22.0f * sb) * walk;   /* degrees */
    float bob     = fabsf(sinf(f->gait)) * (0.05f + 0.035f * sb) * walk;
    float breathe = sinf(f->anim_time * 2.0f) * 0.012f * (1.0f - walk);
    float lean    = walk * (6.0f + 14.0f * sb);             /* lean into the run */
    float arm_up  = -22.0f * sb * walk;                     /* elbows come up   */

    /* Limb angles. Negative pitch swings a limb FORWARD (the model faces +z
     * in its own local space). */
    float leg_l, leg_r, arm_l, arm_r;

    if (f->grounded) {
        leg_l =  swing;
        leg_r = -swing;
        arm_l = -swing * (0.8f + 0.3f * sb) + arm_up;
        arm_r =  swing * (0.8f + 0.3f * sb) + arm_up;
    } else {
        /* Airborne pose: tuck the legs and throw the arms up, biased by
         * whether we are still rising or already falling. */
        float t = fmaxf(-1.0f, fminf(1.0f, f->vy / JUMP_SPEED));
        leg_l = -34.0f + t * 16.0f;
        leg_r = -12.0f - t * 14.0f;
        arm_l = -78.0f - t * 24.0f;
        arm_r = -62.0f - t * 30.0f;
        bob = 0.0f;
        lean = 6.0f - t * 6.0f;
    }

    /* --- attack pose ----------------------------------------------------- */
    /*
     * One scalar off the simulation drives the entire swing: negative while
     * winding up, 1 at the moment of impact, 0 at rest. Every angle below is
     * just that curve times an amplitude, which is how three attacks that look
     * genuinely different come out of one block of code.
     */
    float twist = 0.0f;                    /* upper body rotation about the spine */
    float s = f->attack_strike;

    if (s != 0.0f) {
        /* The walk cycle steps aside in proportion to how much of the pose the
         * swing currently owns, so punching mid-stride does not fight itself. */
        float w = fminf(fabsf(s), 1.0f);
        arm_l *= (1.0f - w);
        arm_r *= (1.0f - w);

        switch (f->attack_index) {
        case 0:   /* jab: lead hand snaps out, off hand stays up as a guard */
            arm_r += s * -115.0f;
            arm_l += s *  -30.0f;
            twist  = s *  -13.0f;
            lean  += s *    7.0f;
            leg_r += s *  -14.0f;
            break;

        case 1:   /* cross: the other hand, with the shoulder turned into it */
            arm_l += s * -125.0f;
            arm_r += s *  -22.0f;
            twist  = s *   16.0f;
            lean  += s *    9.0f;
            leg_l += s *  -16.0f;
            break;

        default:  /* finisher: both hands, whole body committed behind it */
            arm_l += s * -100.0f;
            arm_r += s * -100.0f;
            lean  += s *   20.0f;
            leg_l += s *  -20.0f;
            leg_r += s *   10.0f;
            break;
        }
    }

    rlPushMatrix();
        rlTranslatef(f->x, f->y + bob + breathe, f->z);
        rlRotatef(f->facing * RAD2DEGF, 0.0f, 1.0f, 0.0f);

        /* legs stay in the movement frame -- only the upper body twists ---- */
        draw_limb(-0.14f, HIP_Y, 0.0f, leg_l, LEG_LEN, LEG_THICK, body);
        draw_limb( 0.14f, HIP_Y, 0.0f, leg_r, LEG_LEN, LEG_THICK, body);

        /* Rotating torso, head and arms together about the spine is what makes
         * a punch look thrown rather than poked: the shoulder travels with the
         * arm instead of the arm swinging off a fixed body. */
        rlRotatef(twist, 0.0f, 1.0f, 0.0f);

        /* torso ---------------------------------------------------------- */
        rlPushMatrix();
            rlTranslatef(0.0f, HIP_Y + TORSO_H * 0.5f, 0.0f);
            rlRotatef(lean, 1.0f, 0.0f, 0.0f);
            DrawCube((Vector3){0}, TORSO_W, TORSO_H, TORSO_D, body);
            DrawCubeWires((Vector3){0}, TORSO_W, TORSO_H, TORSO_D, Fade(BLACK, 0.35f));
        rlPopMatrix();

        /* head ----------------------------------------------------------- */
        DrawCube((Vector3){ 0.0f, HEAD_Y, 0.0f },
                 HEAD_SIZE, HEAD_SIZE, HEAD_SIZE, accent);
        DrawCubeWires((Vector3){ 0.0f, HEAD_Y, 0.0f },
                      HEAD_SIZE, HEAD_SIZE, HEAD_SIZE, Fade(BLACK, 0.4f));
        /* a small nose block so you can always tell which way he is facing */
        DrawCube((Vector3){ 0.0f, HEAD_Y, HEAD_SIZE * 0.5f + 0.04f },
                 0.12f, 0.12f, 0.08f, RAYWHITE);

        /* arms ------------------------------------------------------------ */
        draw_limb(-(TORSO_W * 0.5f + ARM_THICK * 0.5f), SHOULDER_Y, 0.0f,
                  arm_l, ARM_LEN, ARM_THICK, accent);
        draw_limb( (TORSO_W * 0.5f + ARM_THICK * 0.5f), SHOULDER_Y, 0.0f,
                   arm_r, ARM_LEN, ARM_THICK, accent);

    rlPopMatrix();

    /* Contact shadow -- a cheap flat disc, but it anchors the model to the
     * ground far better than lighting would at this cost. It shrinks and fades
     * with height, which is most of what sells the jump. */
    float h = fmaxf(0.0f, f->y);
    float shrink = 1.0f / (1.0f + h * 0.55f);
    DrawCylinder((Vector3){ f->x, 0.015f, f->z },
                 0.42f * shrink, 0.42f * shrink, 0.0f, 20,
                 Fade(BLACK, 0.22f * shrink));
}

/* ------------------------------- arena ----------------------------------- */

static void draw_arena(void)
{
    DrawCylinder((Vector3){ 0.0f, -0.05f, 0.0f },
                 ARENA_RADIUS, ARENA_RADIUS, 0.05f, 64,
                 (Color){ 46, 52, 64, 255 });

    /* rim */
    for (int i = 0; i < 3; i++) {
        DrawCircle3D((Vector3){ 0.0f, 0.02f + i * 0.004f, 0.0f },
                     ARENA_RADIUS - i * 0.03f,
                     (Vector3){ 1.0f, 0.0f, 0.0f }, 90.0f,
                     (Color){ 129, 161, 193, 255 });
    }

    /* centre mark, gives a sense of scale and motion while walking */
    DrawCircle3D((Vector3){ 0.0f, 0.02f, 0.0f }, 2.0f,
                 (Vector3){ 1.0f, 0.0f, 0.0f }, 90.0f, Fade(SKYBLUE, 0.35f));

    rlPushMatrix();
        rlTranslatef(0.0f, 0.01f, 0.0f);
        DrawGrid(26, 1.0f);
    rlPopMatrix();
}

/* -------------------------------- HUD ------------------------------------ */

/* Stamina bar: bottom centre, above the help line. It stays dim while full and
 * idle so it does not nag, and lights up the moment it matters. */
static void draw_stamina(const Fighter *f, const Cheats *cheats)
{
    const float bw = 300.0f, bh = 10.0f;
    float bx = GetScreenWidth() * 0.5f - bw * 0.5f;
    float by = (float)GetScreenHeight() - 56.0f;

    bool infinite = cheats && cheats->unlimited_sprint;
    bool idle_full = (!infinite && f->stamina >= STAMINA_MAX && !f->sprinting);
    float a = idle_full ? 0.30f : 1.0f;

    float t = infinite ? 1.0f : (f->stamina / STAMINA_MAX);

    Color fill = infinite      ? (Color){ 235, 203, 139, 255 }   /* amber: cheat */
               : f->exhausted  ? (Color){ 191,  97, 106, 255 }   /* red: locked  */
               : (t < 0.3f)    ? (Color){ 235, 203, 139, 255 }   /* amber: low   */
                               : (Color){ 136, 192, 208, 255 };  /* normal       */

    DrawRectangleRounded((Rectangle){ bx, by, bw, bh }, 1.0f, 4, Fade(BLACK, 0.5f * a));
    if (t > 0.0f)
        DrawRectangleRounded((Rectangle){ bx, by, bw * t, bh }, 1.0f, 4, Fade(fill, a));
    DrawRectangleRoundedLines((Rectangle){ bx, by, bw, bh }, 1.0f, 4,
                              Fade(RAYWHITE, 0.25f * a));

    /* The exhaustion lockout needs to be legible, or it just reads as "sprint
     * randomly stopped working". */
    if (!infinite && f->exhausted) {
        const char *s = "EXHAUSTED";
        DrawText(s, GetScreenWidth() / 2 - MeasureText(s, 14) / 2,
                 (int)by - 20, 14, (Color){ 191, 97, 106, 255 });
    } else if (infinite) {
        const char *s = "UNLIMITED SPRINT";
        DrawText(s, GetScreenWidth() / 2 - MeasureText(s, 14) / 2,
                 (int)by - 20, 14, (Color){ 235, 203, 139, 255 });
    }
}

void render_hud(const World *w, float alpha, const HudInfo *info,
                const Cheats *cheats)
{
    int fps_limit = info->fps_limit;
    bool vsync = info->vsync;

    int fps = GetFPS();
    float ms = GetFrameTime() * 1000.0f;

    DrawRectangle(10, 10, 250, 114, Fade(BLACK, 0.55f));
    DrawRectangleLines(10, 10, 250, 114, Fade(RAYWHITE, 0.25f));

    DrawText(TextFormat("%i FPS", fps), 22, 20, 26,
             fps >= 144 ? GREEN : (fps >= 60 ? YELLOW : RED));
    DrawText(TextFormat("frame  %.3f ms", (double)ms), 22, 50, 14, RAYWHITE);
    DrawText(TextFormat("sim    %i Hz   tick %llu", TICK_HZ,
                        (unsigned long long)w->tick), 22, 68, 14, RAYWHITE);
    DrawText(TextFormat("alpha  %.2f", (double)alpha), 22, 86, 14, GRAY);
    DrawText(TextFormat("cap    %s%s",
                        fps_limit > 0 ? TextFormat("%i", fps_limit) : "unlimited",
                        vsync ? "  +vsync" : ""),
             22, 104, 14, GRAY);

    /* crosshair-free reticle: a subtle dot, enough to orient the mouselook */
    DrawCircle(GetScreenWidth() / 2, GetScreenHeight() / 2, 2.0f,
               Fade(RAYWHITE, 0.35f));

    draw_stamina(&w->player, cheats);

    const char *help = TextFormat(
        "WASD move   Shift sprint   Space jump   LMB attack   %s cheats   Esc menu",
        info->cheat_key ? info->cheat_key : "Tab");
    int tw = MeasureText(help, 14);
    DrawText(help, GetScreenWidth() / 2 - tw / 2, GetScreenHeight() - 26, 14,
             Fade(RAYWHITE, 0.7f));
}

/* ------------------------------ frame stages ----------------------------- */

void render_begin(void)
{
    BeginDrawing();
    ClearBackground((Color){ 24, 26, 33, 255 });
}

void render_end(void)
{
    EndDrawing();
}

void render_world(const World *prev, const World *curr, float alpha)
{
    /* Interpolate the simulation state into a smooth visual state. */
    Fighter p = fighter_lerp(&prev->player, &curr->player, alpha);

    float dt = GetFrameTime();

    /* Aim at the character's chest, following y only partly so a jump does not
     * yank the whole view upward. */
    g_cam_look.x = smooth(g_cam_look.x, p.x, 12.0f, dt);
    g_cam_look.z = smooth(g_cam_look.z, p.z, 12.0f, dt);
    g_cam_look.y = smooth(g_cam_look.y, 1.2f + p.y * 0.5f, 8.0f, dt);

    /* Ease the lateral aim offset, then drop the request. The front end asserts
     * it every frame it draws, so gameplay -- which never does -- decays back to
     * a centred aim on its own, without needing to say so. */
    g_look_side = smooth(g_look_side, g_look_side_want, 5.0f, dt);
    g_look_side_want = 0.0f;

    /* Screen-right in world terms is (cos yaw, 0, -sin yaw); aiming that far to
     * the fighter's left puts him that far right of centre on screen. */
    Vector3 look = g_cam_look;
    look.x -= cosf(g_yaw) * g_look_side;
    look.z += sinf(g_yaw) * g_look_side;

    /* Spherical orbit around the look point. */
    float cp = cosf(g_pitch), sp = sinf(g_pitch);
    g_cam.target   = look;
    g_cam.position = (Vector3){
        look.x + sinf(g_yaw) * cp * CAM_DIST,
        look.y + sp * CAM_DIST,
        look.z + cosf(g_yaw) * cp * CAM_DIST
    };

    BeginMode3D(g_cam);
        draw_arena();
        draw_fighter(&p, (Color){ 94, 129, 172, 255 },
                         (Color){ 235, 203, 139, 255 });
    EndMode3D();
}
