#include "game.h"
#include "hero.h"
#include "model.h"

#include "raylib.h"
#include "rlgl.h"

#include <math.h>

#define PI_F 3.14159265358979323846f
#define TAU_F (2.0f * PI_F)
#define RAD2DEGF (180.0f / PI_F)

/* The camera lives here rather than in the world because it is a presentation
 * concern -- it must never affect the simulation. */
static Camera3D g_cam;
static Vector3  g_cam_look;      /* smoothed point the camera aims at */

#define PITCH_DEFAULT 0.42f      /* radians above the horizon ~24 deg     */
#define PITCH_MIN 0.06f          /* just above ground level ~3.5 deg      */
#define PITCH_MAX 1.30f          /* near straight down     ~74 deg        */
/* Close enough that the fighter fills about a third of the frame height, the
 * over-the-shoulder distance the third-person shooters use. Pulling back past
 * this trades the world you are looking at for empty floor around him. */
#define CAM_DIST  4.1f

/* How close and far the inspection view may pull. Gameplay always sits at
 * CAM_DIST -- the zoom is a front-end affordance, not a camera setting. The
 * range is what a whole model needs to be looked at, so its floor is well short
 * of clipping into one and its ceiling still frames a Dota hero's silhouette. */
#define ZOOM_MIN  -1.2f
#define ZOOM_MAX   8.5f

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

/* Gameplay uses the same mechanism for the opposite reason: parking the fighter
 * left of centre means the half of the frame you are running into is the half
 * he is not standing in. Negative, because a positive offset aims left of him
 * and so pushes him right. This is the resting value -- the front end overrides
 * it while it draws, and the view eases back here the moment it stops. */
#define PLAY_LOOK_SIDE (-0.8f)

static float g_look_side     = PLAY_LOOK_SIDE;   /* current offset, eased */
static float g_look_side_want = PLAY_LOOK_SIDE;  /* re-asserted per frame */

/* Inspection zoom, in world units added to CAM_DIST. Held while the customize
 * pages ask for it and eased away once they stop, the same one-frame latch the
 * lateral offset uses -- so gameplay is never left zoomed. */
static float g_zoom;
static float g_zoom_want;
static bool  g_inspecting;

/* ------------------------------------------------------------------------ */

void render_init(void)
{
    g_cam_look = (Vector3){ 0.0f, 1.2f, 0.0f };

    g_cam.position   = (Vector3){ 0.0f, sinf(PITCH_DEFAULT) * CAM_DIST,
                                        cosf(PITCH_DEFAULT) * CAM_DIST };
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

/*
 * The yaw the camera is LOOKING along, in the simulation's convention where a
 * yaw of 0 means +z.
 *
 * These two differ by half a turn and it is easy to miss. g_yaw places the
 * camera on the orbit -- at g_yaw = 0 it sits at +z -- so the direction it
 * faces is -z, which is a fighter facing of PI. Feeding g_yaw straight in as an
 * aim sends every shot out of the back of your head, which is exactly what it
 * did until this existed.
 */
float render_camera_aim_yaw(void)
{
    float a = g_yaw + PI_F;
    if (a >  PI_F) a -= 2.0f * PI_F;
    if (a < -PI_F) a += 2.0f * PI_F;
    return a;
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
void render_camera_idle_orbit(float dt, bool drift)
{
    /* The customize pages turn the drift off: once you are inspecting a model,
     * a camera that keeps wandering is fighting you. */
    if (drift) {
        g_yaw += IDLE_ORBIT_RATE * dt;
        if (g_yaw > PI_F) g_yaw -= 2.0f * PI_F;
        g_pitch = smooth(g_pitch, PITCH_DEFAULT, 2.5f, dt);
    }
    g_look_side_want = IDLE_LOOK_SIDE;
}

void render_camera_inspect(float dx, float dy, float wheel, float sensitivity)
{
    render_camera_look(dx, dy, sensitivity);

    g_zoom_want = g_zoom_want - wheel;    /* wheel up pulls the camera in */
    if (g_zoom_want < ZOOM_MIN) g_zoom_want = ZOOM_MIN;
    if (g_zoom_want > ZOOM_MAX) g_zoom_want = ZOOM_MAX;
    g_inspecting = true;
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

/* With one box per leg there are no knees to bend, so crouching is drawn by
 * SHORTENING the legs and dropping everything that rides on top of them by the
 * same amount. Cheap, and it reads correctly from every camera angle. */
#define CROUCH_DROP   0.42f   /* fraction of leg length folded away          */
#define CROUCH_LEAN  17.0f    /* degrees the torso tips forward over the feet */

/* How fast the push-off pose leaves after the feet do. */
#define TAKEOFF_FADE  0.045f  /* seconds                                     */

/*
 * `roll` swings the limb out to the side, `pitch` fore and aft. The side-to-side
 * axis earns its keep: the camera usually sits behind the character, and from
 * there a leg tucked forward is almost entirely foreshortened -- so without a
 * lateral component every airborne pose would read as "legs straight down".
 */
static void draw_limb(float px, float py, float pz,
                      float pitch_deg, float roll_deg,
                      float len, float thick, Color fill)
{
    rlPushMatrix();
        rlTranslatef(px, py, pz);
        rlRotatef(roll_deg, 0.0f, 0.0f, 1.0f);
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

    /*
     * The airborne phase, as three overlapping weights plus a takeoff accent.
     * Computed up front because the whole body uses them -- the legs, the arms
     * and how far the figure stretches -- and zeroed on the ground, where a
     * vertical speed of zero would otherwise read as a permanent apex.
     */
    float rise = 0.0f, fall = 0.0f, apex = 0.0f, kick = 0.0f;
    if (!f->grounded) {
        float t = fmaxf(-1.0f, fminf(1.0f, f->vy / JUMP_SPEED));
        rise = fmaxf(0.0f, t);
        fall = fmaxf(0.0f, -t);
        apex = 1.0f - fabsf(t);
        /* The push-off is an accent, not a pose: gone inside a tenth of a
         * second, which is exactly what sells the shove. */
        kick = expf(-f->air_time / TAKEOFF_FADE) * rise;
    }

    /* How far the hips are dropped. Crouching holds them down and a landing
     * pushes them down for a moment; folding both into ONE value means the two
     * never fight over the legs when you land while holding crouch. */
    float dip = fminf(1.0f, f->crouch_blend + f->land_blend * 0.85f);

    /* Takeoff stretches and landing squashes: the same idea at both ends of the
     * jump, and the part of it that reads from any camera angle. */
    float leg_len    = LEG_LEN * (1.0f - CROUCH_DROP * dip + 0.09f * kick);
    float hip_y      = leg_len;
    float shoulder_y = leg_len + TORSO_H - 0.06f;
    float head_y     = leg_len + TORSO_H + HEAD_SIZE * 0.5f;
    float stance     = 0.14f + 0.07f * dip;   /* feet widen for a stable base */

    /* Sprint reshapes the same walk cycle rather than replacing it: a longer
     * stride, a deeper bob, more forward lean, and arms driven up into a pump.
     * The stride FREQUENCY needs no special case -- gait advances by distance
     * travelled, so moving faster already steps faster. */
    float sb = f->sprint_blend;                             /* 0..1, eased */

    /* Crouching shortens the stride into a shuffle rather than stopping it. */
    float swing   = sinf(f->gait) * (42.0f + 22.0f * sb) * walk * (1.0f - 0.45f * dip);
    float bob     = fabsf(sinf(f->gait)) * (0.05f + 0.035f * sb) * walk * (1.0f - dip);
    float breathe = sinf(f->anim_time * 2.0f) * 0.012f * (1.0f - walk);
    float lean    = walk * (6.0f + 14.0f * sb) + CROUCH_LEAN * dip;
    float arm_up  = -22.0f * sb * walk;                     /* elbows come up   */

    /* Limb angles. Negative pitch swings a limb FORWARD (the model faces +z
     * in its own local space); roll is how far it splays OUT to the side. */
    float leg_l, leg_r, arm_l, arm_r;
    float leg_roll, arm_roll;

    if (f->grounded) {
        leg_l =  swing;
        leg_r = -swing;
        arm_l = -swing * (0.8f + 0.3f * sb) + arm_up;
        arm_r =  swing * (0.8f + 0.3f * sb) + arm_up;

        /* Landing: the arms come forward and out to catch the weight while the
         * dip above folds the legs. Fades out with the squash. */
        arm_l -= 40.0f * f->land_blend;
        arm_r -= 34.0f * f->land_blend;

        /* Crouching pushes the knees out; the arms always clear the body a
         * little, and more so at a sprint. */
        leg_roll = 12.0f * dip;
        arm_roll =  4.0f + 7.0f * sb + 16.0f * f->land_blend + 8.0f * dip;
    } else {
        /*
         * Airborne. The four phases the simulation names -- takeoff, rise,
         * apex, fall -- are built here out of the CONTINUOUS weights above
         * rather than switched on AnimState, so they flow into one another
         * instead of popping the moment vy crosses a threshold.
         *
         *   takeoff  the body stretches, the pushing leg trails behind
         *   rise     legs driven up under the body, arms thrown overhead
         *   apex     everything opens out: the hang at the top of the arc
         *   fall     legs reach down for the ground, arms trail behind
         */
        leg_l = -30.0f - 26.0f * rise -  8.0f * apex + 24.0f * fall;
        leg_r = -14.0f - 10.0f * rise + 12.0f * apex + 28.0f * fall + 52.0f * kick;
        arm_l = -74.0f - 30.0f * rise - 16.0f * apex + 26.0f * fall;
        arm_r = -60.0f - 26.0f * rise - 24.0f * apex + 32.0f * fall;
        bob   = 0.0f;
        lean  = 7.0f * rise - 11.0f * fall;

        /* Tuck on the way up, open out at the hang, gather again to land. */
        leg_roll =  4.0f + 20.0f * apex +  6.0f * fall - 10.0f * kick;
        arm_roll = 10.0f + 30.0f * apex + 18.0f * fall + 12.0f * rise;
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

        if (f->attack_air) {
            /* One shape for every air swing rather than three. Off the ground
             * there is nothing to plant a jab or a cross against, so the chain
             * still advances -- it just reads as one committed dive with both
             * hands, legs trailing behind the strike.
             *
             * The amplitude stops well short of the ground swings' on purpose:
             * past about -90 the arms come back UP over the head, which reads
             * as a cheer rather than as a strike. */
            arm_l += s * -78.0f;
            arm_r += s * -78.0f;
            arm_roll *= (1.0f - w);      /* both hands drive down the centre */
            lean  += s *  30.0f;
            leg_l += s *  30.0f;
            leg_r += s *  38.0f;
        } else switch (f->attack_index) {
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
        draw_limb(-stance, hip_y, 0.0f, leg_l, -leg_roll, leg_len, LEG_THICK, body);
        draw_limb( stance, hip_y, 0.0f, leg_r,  leg_roll, leg_len, LEG_THICK, body);

        /* Rotating torso, head and arms together about the spine is what makes
         * a punch look thrown rather than poked: the shoulder travels with the
         * arm instead of the arm swinging off a fixed body. */
        rlRotatef(twist, 0.0f, 1.0f, 0.0f);

        /* torso ---------------------------------------------------------- */
        rlPushMatrix();
            rlTranslatef(0.0f, hip_y + TORSO_H * 0.5f, 0.0f);
            rlRotatef(lean, 1.0f, 0.0f, 0.0f);
            DrawCube((Vector3){0}, TORSO_W, TORSO_H, TORSO_D, body);
            DrawCubeWires((Vector3){0}, TORSO_W, TORSO_H, TORSO_D, Fade(BLACK, 0.35f));
        rlPopMatrix();

        /* head ----------------------------------------------------------- */
        DrawCube((Vector3){ 0.0f, head_y, 0.0f },
                 HEAD_SIZE, HEAD_SIZE, HEAD_SIZE, accent);
        DrawCubeWires((Vector3){ 0.0f, head_y, 0.0f },
                      HEAD_SIZE, HEAD_SIZE, HEAD_SIZE, Fade(BLACK, 0.4f));
        /* a small nose block so you can always tell which way he is facing */
        DrawCube((Vector3){ 0.0f, head_y, HEAD_SIZE * 0.5f + 0.04f },
                 0.12f, 0.12f, 0.08f, RAYWHITE);

        /* arms ------------------------------------------------------------ */
        draw_limb(-(TORSO_W * 0.5f + ARM_THICK * 0.5f), shoulder_y, 0.0f,
                  arm_l, -arm_roll, ARM_LEN, ARM_THICK, accent);
        draw_limb( (TORSO_W * 0.5f + ARM_THICK * 0.5f), shoulder_y, 0.0f,
                   arm_r,  arm_roll, ARM_LEN, ARM_THICK, accent);

    rlPopMatrix();

}

/* An imported skin arrives with its own materials and textures, and a tint is a
 * MULTIPLY -- at full strength a mid-tone colour turns a textured model into a
 * dark silhouette. Washing the colour most of the way to white keeps the hue
 * the player picked while leaving the model's own art legible. */
static Color mesh_tint(Color c)
{
    const float wash = 0.55f;
    return (Color){
        (unsigned char)(c.r + (255 - c.r) * wash),
        (unsigned char)(c.g + (255 - c.g) * wash),
        (unsigned char)(c.b + (255 - c.b) * wash),
        255
    };
}

/* The contact shadow belongs to whoever is standing there, box model or
 * imported skin -- a cheap flat disc, but it anchors the character to the
 * ground far better than lighting would at this cost. It shrinks and fades with
 * height, which is most of what sells the jump. */
static void draw_shadow(const Fighter *f)
{
    float h = fmaxf(0.0f, f->y);
    float shrink = 1.0f / (1.0f + h * 0.55f);
    DrawCylinder((Vector3){ f->x, 0.015f, f->z },
                 0.42f * shrink, 0.42f * shrink, 0.0f, 20,
                 Fade(BLACK, 0.22f * shrink));
}

/* ------------------------------- arena ----------------------------------- */

/* ---------------------------------------------------------------------------
 * The ground.
 *
 * The arena is a rule, not a room: the fighters are held inside ARENA_RADIUS,
 * but the ground runs out to GROUND_EXTENT so the eye never finds an edge, and
 * the horizon is the same colour as the sky so the two meet in haze. Nothing
 * out there is simulated -- it is scenery, and it exists so that walking to the
 * rim reads as "far enough" rather than "the world stops here".
 *
 * Every plant is placed by hashing its index. That keeps the field identical
 * from run to run without storing a single position, and without touching the
 * simulation's generator -- scenery must never draw from it, or the renderer
 * would be advancing simulation state (invariant 3).
 * ------------------------------------------------------------------------- */
#define GROUND_EXTENT   260.0f
#define PLANT_COUNT     900
#define PLANT_FIELD     70.0f    /* plants scatter this far from the centre  */

static const Color SKY_COLOR   = { 143, 178, 199, 255 };
static const Color GRASS_NEAR  = {  92, 118,  76, 255 };
static const Color GRASS_FAR   = {  78, 100,  68, 255 };

/* Integer hash -> [0,1). Deterministic, cheap, and entirely the renderer's. */
static float hash01(unsigned int n)
{
    n = (n ^ 61u) ^ (n >> 16);
    n *= 9u;
    n ^= n >> 4;
    n *= 0x27d4eb2du;
    n ^= n >> 15;
    return (float)(n & 0xFFFFFFu) / (float)0x1000000u;
}

static void draw_plant(float x, float z, float y, float scale, Color leaf)
{
    /* Three crossed blades. Cheap, and from any angle at least one is edge-on
     * rather than all of them, which is what stops a field of these reading as
     * a field of flat cards. */
    for (int i = 0; i < 3; i++) {
        float a = (float)i * (PI_F / 3.0f);
        float dx = sinf(a) * 0.09f * scale;
        float dz = cosf(a) * 0.09f * scale;
        DrawTriangle3D((Vector3){ x - dx, y, z - dz },
                       (Vector3){ x + dx, y, z + dz },
                       (Vector3){ x, y + 0.42f * scale, z },
                       leaf);
        /* Backface: raylib culls, and a blade visible from one side only
         * flickers as the camera swings round it. */
        DrawTriangle3D((Vector3){ x + dx, y, z + dz },
                       (Vector3){ x - dx, y, z - dz },
                       (Vector3){ x, y + 0.42f * scale, z },
                       leaf);
    }
}

/* ------------------------------- the enemy ------------------------------- */

static const Color ENEMY_BODY   = { 176,  78,  74, 255 };
static const Color ENEMY_ACCENT = { 226, 160, 120, 255 };

/* Wash a colour toward white for a moment after a hit. The flash is a Fighter
 * field the simulation sets and decays, so both fighters flash on the same
 * tick they take damage, at any frame rate. */
static Color hurt_tint(Color c, float flash)
{
    if (flash <= 0.0f) return c;
    float t = flash * 0.75f;
    return (Color){
        (unsigned char)(c.r + (255 - c.r) * t),
        (unsigned char)(c.g + (255 - c.g) * t),
        (unsigned char)(c.b + (255 - c.b) * t),
        c.a,
    };
}

/* A bolt in flight: a bright core with a short tail behind it along its own
 * velocity, which is what sells the direction at a glance. */
static void draw_bolts(const World *prev, const World *curr, float alpha)
{
    for (int i = 0; i < BOLT_MAX; i++) {
        const Bolt *b = &curr->bolts[i];
        if (!b->active) continue;

        /* Interpolate only when the slot held the same bolt last tick --
         * otherwise a freshly spawned one would streak in from wherever the
         * previous occupant died. */
        const Bolt *a = &prev->bolts[i];
        bool same = a->active && a->owner == b->owner && a->life > b->life;
        Vector3 pos = same
            ? (Vector3){ a->x + (b->x - a->x) * alpha,
                         a->y + (b->y - a->y) * alpha,
                         a->z + (b->z - a->z) * alpha }
            : (Vector3){ b->x, b->y, b->z };

        Color core = (b->owner == SIDE_PLAYER)
                   ? (Color){ 150, 220, 255, 255 }
                   : (Color){ 255, 170, 120, 255 };

        float inv = 1.0f / BOLT_SPEED;
        Vector3 tail = { pos.x - b->vx * inv * 0.55f, pos.y,
                         pos.z - b->vz * inv * 0.55f };

        DrawCylinderEx(tail, pos, BOLT_RADIUS * 0.35f, BOLT_RADIUS,
                       6, Fade(core, 0.55f));
        DrawSphere(pos, BOLT_RADIUS, core);
        DrawSphere(pos, BOLT_RADIUS * 1.7f, Fade(core, 0.22f));
    }
}

/* A health bar pinned over a fighter's head. Projected by hand rather than
 * billboarded so it stays a constant size on screen -- a bar that shrinks with
 * distance is exactly the thing you cannot read when you need it. */
static void draw_overhead_health(const Fighter *f)
{
    Vector3 head = { f->x, f->y + FIGHTER_HEIGHT + 0.45f, f->z };
    Vector2 s = GetWorldToScreen(head, g_cam);

    /* Behind the camera, or off screen: GetWorldToScreen still returns a
     * point, and drawing it would put a bar in the wrong place entirely. */
    Vector3 to = { head.x - g_cam.position.x, head.y - g_cam.position.y,
                   head.z - g_cam.position.z };
    Vector3 fwd = { g_cam.target.x - g_cam.position.x,
                    g_cam.target.y - g_cam.position.y,
                    g_cam.target.z - g_cam.position.z };
    if (to.x * fwd.x + to.y * fwd.y + to.z * fwd.z <= 0.0f) return;

    const float w = 96.0f, h = 8.0f;
    float x = s.x - w * 0.5f, y = s.y - h;
    float frac = f->health / HEALTH_MAX;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;

    DrawRectangle((int)x - 1, (int)y - 1, (int)w + 2, (int)h + 2,
                  Fade(BLACK, 0.55f));
    DrawRectangle((int)x, (int)y, (int)(w * frac), (int)h,
                  frac > 0.5f ? (Color){ 120, 200, 110, 255 }
                : frac > 0.25f ? (Color){ 226, 192,  90, 255 }
                               : (Color){ 214,  92,  84, 255 });
    DrawRectangleLines((int)x - 1, (int)y - 1, (int)w + 2, (int)h + 2,
                       Fade(RAYWHITE, 0.35f));
}

/*
 * Every horizontal surface here sits at a DIFFERENT height, and that is not
 * fussiness -- it is the whole reason the ground does not shimmer.
 *
 * Two coplanar polygons have equal depth at every pixel they share, so which
 * one wins is decided by float noise and changes as the camera moves: the
 * result is a flicker across the entire overlap. The first cut of this arena
 * put the plain at y=0 and a disc whose top face also landed at exactly y=0,
 * and the two fought over every pixel of a 32-unit circle.
 *
 * So the plain is BELOW the arena floor, and the arena floor is the surface
 * the fighters actually stand on:
 *
 *      y = +0.030   bound ring, centre mark
 *      y = +0.015   shadows
 *      y = +0.002   plant bases
 *      y =  0.000   arena floor  <- fighters stand here
 *      y = -0.060   the far plain
 */
#define GROUND_DROP     0.060f   /* how far the far plain sits below the floor */
#define ARENA_PAD       6.0f     /* floor extends past the bound by this much  */
#define PLANT_BASE      0.002f

static void draw_arena(void)
{
    /* The plain, well past anywhere you can walk, and clearly below the floor
     * so the two can never contest a pixel. */
    DrawPlane((Vector3){ 0.0f, -GROUND_DROP, 0.0f },
              (Vector2){ GROUND_EXTENT, GROUND_EXTENT }, GRASS_FAR);

    /* The arena floor: a low plateau whose top IS y = 0. */
    DrawCylinder((Vector3){ 0.0f, -GROUND_DROP, 0.0f },
                 ARENA_RADIUS + ARENA_PAD, ARENA_RADIUS + ARENA_PAD,
                 GROUND_DROP, 72, GRASS_NEAR);

    /* The bound itself: a worn ring rather than a fence. Concentric rings never
     * overlap each other, so these only need to clear the floor. */
    for (int i = 0; i < 3; i++) {
        DrawCircle3D((Vector3){ 0.0f, 0.030f, 0.0f },
                     ARENA_RADIUS - i * 0.06f,
                     (Vector3){ 1.0f, 0.0f, 0.0f }, 90.0f,
                     (Color){ 196, 186, 150, 190 });
    }

    /* Centre mark -- scale and a sense of motion while walking. */
    DrawCircle3D((Vector3){ 0.0f, 0.030f, 0.0f }, 2.0f,
                 (Vector3){ 1.0f, 0.0f, 0.0f }, 90.0f,
                 Fade((Color){ 214, 208, 180, 255 }, 0.5f));

    for (int i = 0; i < PLANT_COUNT; i++) {
        /* sqrt on the radius keeps the scatter even rather than bunched at the
         * middle, which is what a raw uniform radius would give. */
        float r = sqrtf(hash01((unsigned)i * 3u + 1u)) * PLANT_FIELD;
        float a = hash01((unsigned)i * 7u + 5u) * TAU_F;
        float x = sinf(a) * r, z = cosf(a) * r;

        float t = hash01((unsigned)i * 11u + 3u);
        float scale = 0.6f + t * 0.9f;

        /* A few are taller and bluer: shrubs among the grass. */
        Color leaf = (t > 0.88f)
                   ? (Color){ 104, 128, 92, 255 }
                   : (Color){ (unsigned char)(96 + t * 40.0f),
                              (unsigned char)(124 + t * 34.0f),
                              (unsigned char)(72 + t * 22.0f), 255 };
        /* Just clear of the floor: a blade whose base edge lay exactly on it
         * would fight along that edge. Outside the plateau they stand a few
         * centimetres proud of the plain, which is invisible at that range. */
        draw_plant(x, z, PLANT_BASE, (t > 0.88f) ? scale * 1.9f : scale, leaf);
    }
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

    /* Mark what a jump costs. Without it the jump simply stops working at some
     * invisible point on the bar; with it you can see the last one coming. */
    if (!infinite) {
        float jx = bx + bw * (JUMP_STAMINA_COST / STAMINA_MAX);
        bool  can_jump = !f->exhausted && f->stamina >= JUMP_STAMINA_COST;
        DrawRectangle((int)jx, (int)by - 3, 2, (int)bh + 6,
                      Fade(can_jump ? RAYWHITE : (Color){ 191, 97, 106, 255 },
                           0.75f * a));
    }

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

    DrawRectangle(10, 10, 250, 132, Fade(BLACK, 0.55f));
    DrawRectangleLines(10, 10, 250, 132, Fade(RAYWHITE, 0.25f));

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
    /* What the SIMULATION thinks it is doing, which is not always what the pose
     * looks like -- the poses blend, the states do not. */
    DrawText(TextFormat("anim   %s", ANIM_NAMES[w->player.anim]), 22, 122, 14,
             (Color){ 136, 192, 208, 255 });

    /* --- health ---------------------------------------------------------- */
    {
        const Fighter *p = &w->player;
        int sw = GetScreenWidth(), sh = GetScreenHeight();
        const int bw = 300, bh = 18;
        int bx = sw / 2 - bw / 2, by = sh - 96;

        float frac = p->health / HEALTH_MAX;
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;

        DrawRectangle(bx - 2, by - 2, bw + 4, bh + 4, Fade(BLACK, 0.6f));
        DrawRectangle(bx, by, (int)(bw * frac), bh,
                      frac > 0.5f ? (Color){ 120, 200, 110, 255 }
                    : frac > 0.25f ? (Color){ 226, 192,  90, 255 }
                                   : (Color){ 214,  92,  84, 255 });
        DrawRectangleLines(bx - 2, by - 2, bw + 4, bh + 4, Fade(RAYWHITE, 0.35f));
        DrawText(TextFormat("%i", (int)(p->health + 0.5f)), bx + bw + 10,
                 by + 1, 16, RAYWHITE);

        /* Dead: say so, and count down, because a respawn you cannot time is
         * indistinguishable from a hang. */
        if (!p->alive) {
            const char *msg = "DOWN";
            int tw = MeasureText(msg, 52);
            DrawText(msg, sw / 2 - tw / 2, sh / 2 - 70, 52,
                     (Color){ 226, 108, 100, 255 });
            const char *sub = TextFormat("back in %.1f", (double)p->respawn);
            int sw2 = MeasureText(sub, 22);
            DrawText(sub, sw / 2 - sw2 / 2, sh / 2 - 12, 22,
                     Fade(RAYWHITE, 0.85f));
        }
    }

    /* crosshair-free reticle: a subtle dot, enough to orient the mouselook */
    DrawCircle(GetScreenWidth() / 2, GetScreenHeight() / 2, 2.0f,
               Fade(RAYWHITE, 0.35f));

    draw_stamina(&w->player, cheats);

    const char *help = TextFormat(
        "WASD move   Shift sprint   %s crouch   Space jump   LMB attack   "
        "%s cheats   Esc menu",
        info->crouch_key ? info->crouch_key : "L Ctrl",
        info->cheat_key  ? info->cheat_key  : "Tab");
    int tw = MeasureText(help, 14);
    DrawText(help, GetScreenWidth() / 2 - tw / 2, GetScreenHeight() - 26, 14,
             Fade(RAYWHITE, 0.7f));
}

/* ------------------------------ frame stages ----------------------------- */

void render_begin(void)
{
    BeginDrawing();
    /* The horizon is this colour too, so ground and sky meet in haze rather
     * than at a visible edge. */
    ClearBackground(SKY_COLOR);
}

void render_end(void)
{
    EndDrawing();
}

void render_world(const World *prev, const World *curr, float alpha,
                  const Hero *hero)
{
    /* Interpolate the simulation state into a smooth visual state. */
    Fighter p = fighter_lerp(&prev->player, &curr->player, alpha);

    float dt = GetFrameTime();

    /* Aim at the character's chest, following y only partly so a jump does not
     * yank the whole view upward. */
    g_cam_look.x = smooth(g_cam_look.x, p.x, 12.0f, dt);
    g_cam_look.z = smooth(g_cam_look.z, p.z, 12.0f, dt);
    g_cam_look.y = smooth(g_cam_look.y, 1.2f + p.y * 0.5f, 8.0f, dt);

    /* Ease the lateral aim offset, then drop the request back to the gameplay
     * framing. The front end asserts its own every frame it draws, so gameplay
     * -- which never does -- returns to the over-the-shoulder offset on its own,
     * without needing to say so. */
    g_look_side = smooth(g_look_side, g_look_side_want, 5.0f, dt);
    g_look_side_want = PLAY_LOOK_SIDE;

    /* Same latch for the inspection zoom: held while a customize page keeps
     * asking, released the moment it stops, so play never starts zoomed. */
    g_zoom = smooth(g_zoom, g_inspecting ? g_zoom_want : 0.0f, 7.0f, dt);
    if (!g_inspecting) g_zoom_want = 0.0f;
    g_inspecting = false;

    /* Screen-right in world terms is (cos yaw, 0, -sin yaw); aiming that far to
     * the fighter's left puts him that far right of centre on screen. */
    Vector3 look = g_cam_look;
    look.x -= cosf(g_yaw) * g_look_side;
    look.z += sinf(g_yaw) * g_look_side;

    /* Spherical orbit around the look point. */
    float dist = CAM_DIST + g_zoom;
    float cp = cosf(g_pitch), sp = sinf(g_pitch);
    g_cam.target   = look;
    g_cam.position = (Vector3){
        look.x + sinf(g_yaw) * cp * dist,
        look.y + sp * dist,
        look.z + cosf(g_yaw) * cp * dist
    };

    /* An imported skin replaces the box fighter when one is selected and
     * loadable; otherwise the built-in model draws, so the game always has
     * somebody to play as. */
    Fighter e = fighter_lerp(&prev->enemy, &curr->enemy, alpha);

    bool skin = model_active(hero->model);
    if (skin) model_animate(&p, dt);

    BeginMode3D(g_cam);
        draw_arena();

        if (p.alive) {
            if (!skin || !model_draw(&p, mesh_tint(hero_body(hero))))
                draw_fighter(&p, hero_body(hero), hero_accent(hero));
            draw_shadow(&p);
        }

        /*
         * The opponent is always the built-in box fighter, never the imported
         * skin. Not a style choice: model.c holds ONE loaded model with ONE
         * playhead (§12), so drawing two fighters through it would give them
         * the same pose on the same frame. A box that moves correctly beats a
         * hero that mirrors you.
         */
        if (e.alive) {
            draw_fighter(&e, hurt_tint(ENEMY_BODY, e.hurt_flash),
                         ENEMY_ACCENT);
            draw_shadow(&e);
        }

        draw_bolts(prev, curr, alpha);
    EndMode3D();

    /* Health above the head, projected after the 3D pass so it is flat to the
     * screen and always legible. */
    if (e.alive) draw_overhead_health(&e);
}
