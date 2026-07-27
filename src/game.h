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

/* ---------------------------------------------------------------------------
 * How big a fighter is. One number, owned by the SIMULATION.
 *
 * Every fighter is this tall, always. Appearance must never change it: an
 * imported skin is scaled to this height when it loads, precisely so that
 * picking a bigger model cannot buy you a bigger body -- and so that when
 * hitboxes arrive they measure this constant rather than whatever mesh happens
 * to be drawn. A skin is a costume, not a stat.
 * ------------------------------------------------------------------------- */
#define FIGHTER_HEIGHT 1.90f

/* Jump. Apex = JUMP_SPEED^2 / (2 * GRAVITY) ~= 1.33 units, on a character
 * about 1.9 units tall. Air time ~= 0.67s -- snappy rather than floaty.
 * Shared because the renderer normalises the airborne pose by JUMP_SPEED. */
#define GRAVITY     24.0f
#define JUMP_SPEED   8.0f

/* Walking top speed. Simulation-owned; shared because imported movement clips
 * are paced against it -- a clip plays at its authored rate at exactly this
 * ground speed (see model.c). */
#define MOVE_MAX     6.0f

/* Sprint: 18% above walking top speed. */
#define SPRINT_MULT  1.18f

/* Stamina is shared with the HUD, which draws the bar. */
#define STAMINA_MAX          100.0f
#define STAMINA_EXHAUST_AT    30.0f  /* must recover to here before sprinting again */

/* A jump spends from the SAME pool as sprinting, so mobility is one budget
 * rather than two: you cannot hop your way across the arena while the bar is
 * empty, and every jump is stride you are not going to run. Five jumps from
 * full. Shared because the HUD marks the threshold on the bar. */
#define JUMP_STAMINA_COST     20.0f

/* Melee attacks come in a fixed-order chain rather than a random pick -- see
 * the long comment in fighter.c for why. The renderer needs the count so it can
 * pose each swing differently. */
#define ATTACK_CHAIN 3

/* ------------------------------- animation ------------------------------- */

/*
 * What the fighter is doing, as the SIMULATION sees it. The renderer does not
 * switch on this: it builds poses out of continuous quantities (velocity, the
 * eased blends, the strike curve) so the phases flow into each other instead of
 * popping at the boundaries. This enum is the simulation naming those same
 * phases -- for the HUD readout, for tests, and for the hit reactions that will
 * need to know what they interrupted.
 */
typedef enum AnimState {
    ANIM_IDLE = 0,
    ANIM_WALK,
    ANIM_SPRINT,
    ANIM_CROUCH,
    ANIM_JUMP_TAKEOFF,   /* the push-off, first fraction of a second airborne */
    ANIM_JUMP_RISE,
    ANIM_JUMP_APEX,      /* the hang at the top, where vy is near zero        */
    ANIM_JUMP_FALL,
    ANIM_LAND,           /* the squash on touchdown; never takes control away */
    ANIM_ATTACK,
    ANIM_HURT,
    ANIM_SPECIAL,
    ANIM_COUNT
} AnimState;

/* Names for the HUD readout, indexed by AnimState. */
extern const char *const ANIM_NAMES[ANIM_COUNT];

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
    float air_time;     /* seconds since the feet left the ground            */
    float land_blend;   /* 0..1 landing squash, set by impact, decays to 0   */

    /* sprint / stamina */
    bool  sprinting;
    bool  exhausted;    /* hit zero; locked out until STAMINA_EXHAUST_AT     */
    float stamina;
    float regen_delay;  /* seconds left before stamina starts coming back    */
    float sprint_blend; /* 0..1 eased, drives the animation only             */

    /* crouch */
    bool  crouching;
    float crouch_blend; /* 0..1 eased, drives the animation only             */

    /* melee attack chain */
    bool  attacking;
    int   attack_index;    /* which swing of the chain, 0..ATTACK_CHAIN-1     */
    float attack_time;     /* seconds into the current swing                  */
    float attack_strike;   /* pose driver: <0 winding up, 1 = impact, 0 rest  */
    bool  attack_buffered; /* a press that landed too early to chain yet      */
    bool  attack_air;      /* this swing started airborne -- latched, so it   */
                           /* does not change shape when you land mid-swing   */
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
    bool  crouch;       /* held                                             */
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

/* Seconds into the current swing at which the strike lands (the end of the
 * ACTIVE window), or 0 when not attacking. The renderer anchors an imported
 * attack clip's contact frame to this moment, so the hit still reads on the
 * simulation's hit window however long the clip was authored. */
float fighter_attack_impact_time(const Fighter *f);

void  world_init(World *w);
void  world_tick(World *w, Input in, const Cheats *cheats);

/* render.c
 * Split into stages so main.c can compose a frame: world, HUD, then an
 * optional menu overlay, all inside one Begin/EndDrawing pair. */
void  render_init(void);
void  render_begin(void);
/* `hero` is presentation only -- which skin, colours and size to draw with. It
 * never reaches the simulation. */
struct Hero;
void  render_world(const World *prev, const World *curr, float alpha,
                   const struct Hero *hero);
/* Everything the HUD needs that is not part of the simulation. Passed as a
 * struct so adding a readout later does not mean another positional argument. */
typedef struct HudInfo {
    int         fps_limit;      /* 0 = unlimited */
    bool        vsync;
    const char *cheat_key;      /* label for the help line, e.g. "Tab" */
    const char *crouch_key;     /* likewise, e.g. "L Ctrl"             */
} HudInfo;

void  render_hud(const World *w, float alpha, const HudInfo *info,
                 const Cheats *cheats);
void  render_end(void);

/* Third-person orbit camera. The camera owns its own yaw/pitch; main.c feeds
 * it raw mouse deltas and reads the yaw back to make movement camera-relative. */
void  render_camera_look(float dx, float dy, float sensitivity);
float render_camera_yaw(void);

/* Front-end framing: parks the fighter to one side of the menu, and with
 * `drift` set also turns slowly for the title backdrop. Wall-clock seconds --
 * it moves the camera, never the world. */
void  render_camera_idle_orbit(float dt, bool drift);

/* Orbit and zoom by hand, for the customize pages. Call it every frame the page
 * is open, with zeroes when the player is not dragging: the zoom is held only
 * while it is being asked for, and eases back out when it is not. */
void  render_camera_inspect(float dx, float dy, float wheel, float sensitivity);

#endif /* GAME_H */
