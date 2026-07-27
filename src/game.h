#ifndef GAME_H
#define GAME_H

#include <stdbool.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Simulation timing.
 *
 * The whole game logic runs at a FIXED rate (TICK_HZ). Rendering runs as fast
 * as the machine allows and interpolates between the last two simulation
 * states. This is what keeps combat deterministic and frame-rate independent
 * while still letting the screen update at 1000+ FPS.
 * ------------------------------------------------------------------------- */
#define TICK_HZ    120
#define TICK_DT    (1.0f / (float)TICK_HZ)

/* If the process stalls (window drag, breakpoint), never try to catch up more
 * than this much wall time -- otherwise we'd spiral into an endless tick loop. */
#define MAX_FRAME_TIME 0.25f

/* World is a flat disc. Movement happens on the (x, z) ground plane; y is
 * height, driven only by jumping and gravity. */
#define ARENA_RADIUS 12.0f

/* Jump. Apex = JUMP_SPEED^2 / (2 * GRAVITY) ~= 1.33 units, on a character
 * about 1.9 units tall. Air time ~= 0.67s -- snappy rather than floaty.
 * Shared because the renderer normalises the airborne pose by JUMP_SPEED. */
#define GRAVITY     24.0f
#define JUMP_SPEED   8.0f

/* Sprint: 18% above walking top speed. */
#define SPRINT_MULT  1.18f

/* Stamina is shared with the HUD, which draws the bar. */
#define STAMINA_MAX          100.0f
#define STAMINA_EXHAUST_AT    30.0f  /* must recover to here before sprinting again */

/* Melee attacks come in a fixed-order chain rather than a random pick -- see
 * the long comment in fighter.c for why. The renderer needs the count so it can
 * pose each swing differently. */
#define ATTACK_CHAIN 3

/* ------------------------------- animation ------------------------------- */

typedef enum AnimState {
    ANIM_IDLE = 0,
    ANIM_WALK,
    ANIM_SPRINT,
    ANIM_JUMP,
    ANIM_ATTACK,
    ANIM_HURT,
    ANIM_SPECIAL,
    ANIM_COUNT
} AnimState;

/* -------------------------------- cheats --------------------------------- */

typedef struct Cheats {
    bool unlimited_sprint;
} Cheats;

/* ------------------------------- fighter --------------------------------- */

typedef struct Fighter {
    float x, y, z;      /* position; y is height above the floor            */
    float vx, vy, vz;   /* velocity                                         */
    float facing;       /* yaw in radians, 0 = +z                           */

    float gait;         /* walk-cycle phase, advanced by distance travelled  */
    AnimState anim;
    float anim_time;    /* seconds spent in the current animation           */

    bool  grounded;

    /* sprint / stamina */
    bool  sprinting;
    bool  exhausted;    /* hit zero; locked out until STAMINA_EXHAUST_AT     */
    float stamina;
    float regen_delay;  /* seconds left before stamina starts coming back    */
    float sprint_blend; /* 0..1 eased, drives the animation only             */

    /* melee attack chain */
    bool  attacking;
    int   attack_index;    /* which swing of the chain, 0..ATTACK_CHAIN-1     */
    float attack_time;     /* seconds into the current swing                  */
    float attack_strike;   /* pose driver: <0 winding up, 1 = impact, 0 rest  */
    bool  attack_buffered; /* a press that landed too early to chain yet      */
    float chain_grace;     /* seconds the finished chain stays "continuable"  */

    float health;
} Fighter;

/* Per-frame input. move_x/move_z are already in WORLD space -- main.c rotates
 * the raw key directions by the camera yaw before handing them over, so the
 * simulation never needs to know a camera exists. */
typedef struct Input {
    float move_x, move_z;
    bool  attack;       /* edge-triggered: true only on the tick that swings */
    bool  special;
    bool  jump;         /* edge-triggered: true only on the tick that jumps */
    bool  sprint;       /* held                                             */
} Input;

/* ------------------------------- world ----------------------------------- */

typedef struct World {
    Fighter player;
    uint64_t tick;
} World;

/* fighter.c */
void  fighter_init(Fighter *f, float x, float z);
void  fighter_tick(Fighter *f, Input in, const Cheats *cheats);
Fighter fighter_lerp(const Fighter *a, const Fighter *b, float t);

void  world_init(World *w);
void  world_tick(World *w, Input in, const Cheats *cheats);

/* render.c
 * Split into stages so main.c can compose a frame: world, HUD, then an
 * optional menu overlay, all inside one Begin/EndDrawing pair. */
void  render_init(void);
void  render_begin(void);
void  render_world(const World *prev, const World *curr, float alpha);
/* Everything the HUD needs that is not part of the simulation. Passed as a
 * struct so adding a readout later does not mean another positional argument. */
typedef struct HudInfo {
    int         fps_limit;      /* 0 = unlimited */
    bool        vsync;
    const char *cheat_key;      /* label for the help line, e.g. "Tab" */
} HudInfo;

void  render_hud(const World *w, float alpha, const HudInfo *info,
                 const Cheats *cheats);
void  render_end(void);

/* Third-person orbit camera. The camera owns its own yaw/pitch; main.c feeds
 * it raw mouse deltas and reads the yaw back to make movement camera-relative. */
void  render_camera_look(float dx, float dy, float sensitivity);
float render_camera_yaw(void);

#endif /* GAME_H */
