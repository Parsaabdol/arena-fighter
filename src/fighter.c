#include "game.h"

#include <math.h>

#define PI_F 3.14159265358979323846f
#define TAU_F (2.0f * PI_F)

/* Movement feel. Tuned in units/second; the tick rate is factored in below so
 * changing TICK_HZ does not change how the character handles. */
#define MOVE_ACCEL    60.0f   /* how hard we push toward the desired velocity */
#define MOVE_FRICTION 12.0f   /* deceleration when there is no input          */
#define TURN_RATE     14.0f   /* radians/second the model rotates to face travel */

/* GRAVITY, JUMP_SPEED, MOVE_MAX and SPRINT_MULT live in game.h -- the
 * renderer and the imported-skin clip pacing need them. */
#define AIR_CONTROL    0.35f  /* fraction of ground acceleration while airborne */

/* ---------------------------------------------------------------------------
 * Crouch.
 *
 * A held modifier rather than a toggle, and grounded-only: holding it in the
 * air does nothing, but it takes effect the instant you land. It costs speed,
 * which is the whole trade -- and it locks out sprinting, because "crouch-
 * sprinting" would just be walking with extra steps.
 * ------------------------------------------------------------------------- */
#define CROUCH_SPEED_MULT   0.45f
#define CROUCH_ACCEL_MULT   0.70f
#define CROUCH_BLEND_RATE  12.0f   /* how fast the crouch POSE eases in/out  */

/* The band of vertical speed either side of zero that reads as hanging at the
 * top of the arc rather than as rising or falling. */
#define APEX_VY             1.6f

/* Takeoff lasts only as long as the push-off pose does. */
#define TAKEOFF_TIME        0.10f

/* Landing squash. Animation only: a landing that took control away would cost
 * far more in feel than the pose is worth. The blend starts at how hard you
 * hit and decays at a fixed rate, so a small hop settles faster than a drop. */
#define LAND_DECAY_RATE     (1.0f / 0.20f)

/* ---------------------------------------------------------------------------
 * Stamina.
 *
 * The model is the one most shooters and survival games converge on (Hunt:
 * Showdown, Tarkov, Souls-likes): a bar that drains while sprinting, a short
 * delay before it starts refilling, and -- the part that actually matters --
 * an EXHAUSTION LOCKOUT. Running the bar to zero does not just stop you; it
 * locks sprinting out until you have recovered a meaningful chunk back.
 *
 * Without the lockout, the optimal play is to tap sprint on and off forever,
 * hovering at zero stamina and sprinting ~95% of the time. The lockout is what
 * turns "spam shift" into an actual resource decision.
 * ------------------------------------------------------------------------- */
#define STAMINA_DRAIN        25.0f   /* per second sprinting -> 4.0s from full */
#define STAMINA_REGEN        22.0f   /* per second -> ~4.5s for a full refill  */
#define STAMINA_REGEN_DELAY   0.75f  /* seconds after sprinting before regen   */
#define SPRINT_BLEND_RATE     9.0f   /* how fast the sprint POSE eases in/out  */

/* ---------------------------------------------------------------------------
 * Melee attacks.
 *
 * Three swings that CHAIN in a fixed order instead of being picked at random.
 * Random sounds like the way to get variety, but it is the wrong tool here:
 *
 *   - The simulation is deterministic by design (fixed tick + interpolation).
 *     A random draw inside the tick throws that away, and with it any hope of
 *     replays or rollback netcode later.
 *   - With three options, random repeats the same swing back to back a third of
 *     the time, which reads as a bug rather than as variety.
 *   - A chain gets variety AND meaning for free: which swing you see depends on
 *     what you did a moment ago, so the third one lands as a payoff.
 *
 * Each swing has the shape every action game converges on:
 *
 *   STARTUP    wind up. Committed, but nothing has happened yet.
 *   ACTIVE     the hit window. This is where a hitbox will go once there is
 *              something to hit.
 *   RECOVERY   put yourself back together -- and the window in which pressing
 *              attack again continues the chain.
 * ------------------------------------------------------------------------- */

typedef struct AttackDef {
    float startup, active, recovery;   /* seconds */
    float lunge;                       /* forward push during the hit window  */
} AttackDef;

static const AttackDef ATTACKS[ATTACK_CHAIN] = {
    /* 1 - jab       */ { 0.09f, 0.07f, 0.17f, 26.0f },
    /* 2 - cross     */ { 0.11f, 0.08f, 0.21f, 32.0f },
    /* 3 - finisher  */ { 0.19f, 0.10f, 0.36f, 46.0f },
};

/* Once a swing is fully over the chain stays alive a moment longer, so a player
 * attacking at a natural rhythm still reaches swings 2 and 3 instead of being
 * pinned to swing 1 by a few milliseconds. */
#define CHAIN_GRACE   0.22f

/* How far the pose cocks back before the strike, in strike-curve units. */
#define ATTACK_WINDUP 0.40f

/* Fraction of recovery spent held at full extension. Impact lands between two
 * ticks, so without a hold the fully-extended pose is computed but never
 * actually drawn -- and holding the contact pose for a beat is what makes a hit
 * read as a hit rather than as an arm waving past. */
#define IMPACT_HOLD   0.25f

/* Attacking is a commitment: you keep a little steering authority, not a dash. */
#define ATTACK_SPEED_MULT   0.40f
#define ATTACK_CONTROL_MULT 0.30f
#define ATTACK_TURN_MULT    0.30f

/* Swinging in the air is allowed, and it is the same chain -- but you cannot
 * turn a swing into extra hang time, so the lunge is cut to a nudge rather than
 * a step you could ride across the arena. */
#define AIR_LUNGE_MULT      0.45f

static float ease_out(float t) { return 1.0f - (1.0f - t) * (1.0f - t); }
static float ease_in(float t)  { return t * t; }

/*
 * The whole swing collapsed into one number, which is all the renderer needs:
 * eases negative while winding up, accelerates through the hit window to 1 at
 * the moment of impact, then eases back to 0 during recovery. Three attacks out
 * of one curve -- the poses differ only in amplitude.
 */
static float strike_curve(const AttackDef *a, float t)
{
    if (t < a->startup)
        return -ATTACK_WINDUP * ease_out(t / a->startup);

    t -= a->startup;
    if (t < a->active)
        return -ATTACK_WINDUP + (1.0f + ATTACK_WINDUP) * ease_in(t / a->active);

    t -= a->active;
    float hold = a->recovery * IMPACT_HOLD;
    if (t < hold) return 1.0f;

    float u = fminf((t - hold) / (a->recovery - hold), 1.0f);
    return 1.0f - ease_out(u);
}

/* Wrap an angle into (-PI, PI]. */
static float angle_wrap(float a)
{
    while (a >  PI_F) a -= TAU_F;
    while (a <= -PI_F) a += TAU_F;
    return a;
}

/* Shortest-arc interpolation between two angles. Interpolating raw floats
 * makes the character spin the long way round when crossing the PI boundary. */
static float angle_lerp(float a, float b, float t)
{
    return a + angle_wrap(b - a) * t;
}

static float lerpf(float a, float b, float t)
{
    return a + (b - a) * t;
}

const char *const ANIM_NAMES[ANIM_COUNT] = {
    "idle", "walk", "sprint", "crouch",
    "takeoff", "rise", "apex", "fall", "land",
    "attack", "hurt", "special",
};

void fighter_init(Fighter *f, float x, float z)
{
    f->x = x;
    f->y = 0.0f;
    f->z = z;
    f->vx = f->vy = f->vz = 0.0f;
    f->facing = 0.0f;
    f->gait = 0.0f;
    f->anim = ANIM_IDLE;
    f->anim_time = 0.0f;
    f->grounded = true;
    f->air_time = 0.0f;
    f->land_blend = 0.0f;
    f->sprinting = false;
    f->exhausted = false;
    f->stamina = STAMINA_MAX;
    f->regen_delay = 0.0f;
    f->sprint_blend = 0.0f;
    f->crouching = false;
    f->crouch_blend = 0.0f;
    f->attacking = false;
    f->attack_index = 0;
    f->attack_time = 0.0f;
    f->attack_strike = 0.0f;
    f->attack_buffered = false;
    f->attack_air = false;
    f->chain_grace = 0.0f;
    f->struck = false;
    f->attack_aim = 0.0f;
    f->health = HEALTH_MAX;
    f->alive = true;
    f->respawn = 0.0f;
    f->hurt_flash = 0.0f;
}

static void set_anim(Fighter *f, AnimState s)
{
    if (f->anim != s) {
        f->anim = s;
        f->anim_time = 0.0f;
    }
}

void fighter_tick(Fighter *f, Input in, const Cheats *cheats)
{
    const float dt = TICK_DT;
    bool infinite = (cheats && cheats->unlimited_sprint);

    /* --- attack chain ---------------------------------------------------- */
    f->struck = false;      /* one tick only; set below if the strike lands */

    if (f->attacking) {
        const AttackDef *a = &ATTACKS[f->attack_index];
        float chain_from = a->startup + a->active;   /* recovery = combo window */
        float total      = chain_from + a->recovery;

        float was = f->attack_time;
        f->attack_time += dt;

        /* Record the crossing BEFORE the chain below can reset the clock. The
         * combo window opens at exactly the strike, so a buffered press starts
         * the next swing on this same tick -- and anyone reading attack_time
         * afterwards would see 0 and conclude nothing had happened. */
        if (was < chain_from && f->attack_time >= chain_from) f->struck = true;

        /* A press that arrives mid-swing is too early to chain, so remember it
         * and spend it the instant the window opens. This one line of buffering
         * is most of the difference between "responsive" and "the game ate my
         * input" -- without it you have to press on exactly the right tick. */
        if (in.attack) f->attack_buffered = true;

        if (f->attack_buffered && f->attack_time >= chain_from) {
            f->attack_index = (f->attack_index + 1) % ATTACK_CHAIN;
            f->attack_time = 0.0f;
            f->attack_buffered = false;
            f->attack_air = !f->grounded;   /* a new swing, so re-read the feet */
            /* This is a NEW swing, so it gets its own bolt and its own aim.
             * Forgetting these is silent: the chain animates correctly and only
             * the first swing of it ever throws anything. */
            f->attack_aim = in.aim;
        } else if (f->attack_time >= total) {
            f->attacking = false;
            f->attack_buffered = false;
            f->chain_grace = CHAIN_GRACE;
        }
    } else {
        if (f->chain_grace > 0.0f) {
            f->chain_grace -= dt;
            if (f->chain_grace <= 0.0f) f->attack_index = 0;   /* chain lapsed */
        }

        /* Air attacks use the same chain rather than a separate move: the swing
         * you have been building on the ground is the swing you take with you
         * off it, which is one system to reason about instead of two. */
        if (in.attack) {
            /* Still inside the grace window means this continues the previous
             * chain; otherwise it opens a fresh one. */
            f->attack_index = (f->chain_grace > 0.0f)
                            ? (f->attack_index + 1) % ATTACK_CHAIN
                            : 0;
            f->attacking = true;
            f->attack_time = 0.0f;
            f->attack_buffered = false;
            f->attack_air = !f->grounded;
            f->chain_grace = 0.0f;
            f->attack_aim = in.aim;
        }
    }

    if (f->attacking) {
        const AttackDef *a = &ATTACKS[f->attack_index];
        f->attack_strike = strike_curve(a, f->attack_time);

        /* Step into the swing across the hit window. An attack that leaves you
         * rooted to the spot feels weightless no matter how good the pose is. */
        if (f->attack_time >= a->startup &&
            f->attack_time <  a->startup + a->active) {
            float lunge = a->lunge * (f->attack_air ? AIR_LUNGE_MULT : 1.0f);
            f->vx += sinf(f->facing) * lunge * dt;
            f->vz += cosf(f->facing) * lunge * dt;
        }
    } else {
        /* Recovery already returned the curve to 0, so this is continuous. */
        f->attack_strike = 0.0f;
    }

    /* --- jump ------------------------------------------------------------ */
    /* Jumping is not free: it spends from the same bar sprinting does. Being
     * exhausted grounds you completely, which is the point -- run the bar dry
     * and you have spent your mobility, not just your top speed. */
    bool can_afford_jump = infinite
                        || (!f->exhausted && f->stamina >= JUMP_STAMINA_COST);

    if (in.jump && f->grounded && !f->attacking && can_afford_jump) {
        f->vy = JUMP_SPEED;
        f->grounded = false;
        f->air_time = 0.0f;

        if (!infinite) {
            f->stamina -= JUMP_STAMINA_COST;
            f->regen_delay = STAMINA_REGEN_DELAY;
            if (f->stamina <= 0.0f) {
                f->stamina = 0.0f;
                f->exhausted = true;
                f->sprinting = false;
            }
        }
    }

    /* --- crouch ---------------------------------------------------------- */
    /* Grounded-only, so a held crouch does nothing in the air. It reads the
     * ground state from the START of the tick -- contact is not resolved until
     * gravity runs, below -- so a crouch held through a landing comes back one
     * tick later. That is 8ms, and keeping the tick in a single order is worth
     * more than saving it. */
    f->crouching = in.crouch && f->grounded;

    /* --- movement input -------------------------------------------------- */
    float len = sqrtf(in.move_x * in.move_x + in.move_z * in.move_z);
    if (len > 1.0f) {                    /* normalise so diagonals are not faster */
        in.move_x /= len;
        in.move_z /= len;
        len = 1.0f;
    }
    bool moving = (len > 0.001f);

    /* --- sprint decision -------------------------------------------------- */
    if (f->grounded) {
        bool wants = in.sprint && moving && !f->exhausted && !f->attacking
                  && !f->crouching;
        f->sprinting = wants && (infinite || f->stamina > 0.0f);
    }
    /* While airborne we keep whatever sprint state we launched with, so a
     * running jump stays a running jump. */

    /* --- stamina ---------------------------------------------------------- */
    if (infinite) {
        f->stamina = STAMINA_MAX;
        f->exhausted = false;
        f->regen_delay = 0.0f;
    } else if (f->sprinting && f->grounded) {
        f->stamina -= STAMINA_DRAIN * dt;
        f->regen_delay = STAMINA_REGEN_DELAY;

        if (f->stamina <= 0.0f) {
            f->stamina = 0.0f;
            f->exhausted = true;      /* locked out until STAMINA_EXHAUST_AT */
            f->sprinting = false;
        }
    } else {
        if (f->regen_delay > 0.0f) {
            f->regen_delay -= dt;
        } else {
            f->stamina += STAMINA_REGEN * dt;
            if (f->stamina > STAMINA_MAX) f->stamina = STAMINA_MAX;
        }
        if (f->exhausted && f->stamina >= STAMINA_EXHAUST_AT) f->exhausted = false;
    }

    /* --- velocity --------------------------------------------------------- */
    float top_speed = MOVE_MAX * (f->sprinting ? SPRINT_MULT : 1.0f);
    float control   = f->grounded ? 1.0f : AIR_CONTROL;
    float turn_rate = TURN_RATE;

    if (f->attacking) {
        top_speed *= ATTACK_SPEED_MULT;
        control   *= ATTACK_CONTROL_MULT;
        turn_rate *= ATTACK_TURN_MULT;
    }

    if (f->crouching) {
        top_speed *= CROUCH_SPEED_MULT;
        control   *= CROUCH_ACCEL_MULT;
    }

    if (moving) {
        /* Accelerate toward the target velocity rather than snapping to it. */
        float target_vx = in.move_x * top_speed;
        float target_vz = in.move_z * top_speed;
        float k = fminf(MOVE_ACCEL * control * dt, 1.0f);
        f->vx += (target_vx - f->vx) * k;
        f->vz += (target_vz - f->vz) * k;

        /* Face the direction of travel, easing rather than snapping. */
        float want = atan2f(in.move_x, in.move_z);
        f->facing = angle_wrap(angle_lerp(f->facing, want, fminf(turn_rate * dt, 1.0f)));
    } else if (f->grounded) {
        /* No air friction: momentum carries through a jump, which is what
         * makes jumping feel like a commitment. */
        float damp = fmaxf(0.0f, 1.0f - MOVE_FRICTION * dt);
        f->vx *= damp;
        f->vz *= damp;
    }

    f->x += f->vx * dt;
    f->z += f->vz * dt;

    /* --- gravity --------------------------------------------------------- */
    f->vy -= GRAVITY * dt;
    f->y  += f->vy * dt;

    if (f->y <= 0.0f) {
        /* Squash in proportion to how hard we arrived, so a hop off the rim and
         * a drop from the apex do not look like the same landing. */
        if (!f->grounded) {
            float impact = fminf(1.0f, -f->vy / JUMP_SPEED);
            if (impact > f->land_blend) f->land_blend = impact;
        }
        f->y = 0.0f;
        f->vy = 0.0f;
        f->grounded = true;
    }

    if (f->grounded) {
        f->air_time = 0.0f;
        f->land_blend -= LAND_DECAY_RATE * dt;
        if (f->land_blend < 0.0f) f->land_blend = 0.0f;
    } else {
        f->air_time += dt;
        f->land_blend = 0.0f;   /* leaving the ground cancels any settle left */
    }

    /* --- arena bounds ---------------------------------------------------- */
    float d = sqrtf(f->x * f->x + f->z * f->z);
    if (d > ARENA_RADIUS) {
        float nx = f->x / d, nz = f->z / d;
        f->x = nx * ARENA_RADIUS;
        f->z = nz * ARENA_RADIUS;
        /* Kill the outward component of velocity so we slide along the rim
         * instead of sticking to it. */
        float outward = f->vx * nx + f->vz * nz;
        if (outward > 0.0f) {
            f->vx -= outward * nx;
            f->vz -= outward * nz;
        }
    }

    /* --- animation state ------------------------------------------------- */
    float speed = sqrtf(f->vx * f->vx + f->vz * f->vz);

    /* Attacking outranks everything: it is the thing that just took your
     * control away, so it is the thing worth naming, in the air or not. */
    if (f->attacking)                     set_anim(f, ANIM_ATTACK);
    else if (!f->grounded) {
        if (f->air_time < TAKEOFF_TIME)   set_anim(f, ANIM_JUMP_TAKEOFF);
        else if (f->vy >  APEX_VY)        set_anim(f, ANIM_JUMP_RISE);
        else if (f->vy < -APEX_VY)        set_anim(f, ANIM_JUMP_FALL);
        else                              set_anim(f, ANIM_JUMP_APEX);
    }
    else if (f->land_blend > 0.0f)        set_anim(f, ANIM_LAND);
    else if (f->crouching)                set_anim(f, ANIM_CROUCH);
    else if (f->sprinting)                set_anim(f, ANIM_SPRINT);
    else if (speed > 0.25f)               set_anim(f, ANIM_WALK);
    else                                  set_anim(f, ANIM_IDLE);

    /* Ease the sprint and crouch POSES separately from their STATES, so the
     * body eases in and out instead of popping on the tick the key is hit. */
    float want_blend = f->sprinting ? 1.0f : 0.0f;
    f->sprint_blend += (want_blend - f->sprint_blend)
                     * fminf(SPRINT_BLEND_RATE * dt, 1.0f);

    float want_crouch = f->crouching ? 1.0f : 0.0f;
    f->crouch_blend += (want_crouch - f->crouch_blend)
                     * fminf(CROUCH_BLEND_RATE * dt, 1.0f);

    /* Advance the walk cycle by DISTANCE, not by time. This is why the legs
     * stay in sync with the ground at any speed instead of skating -- and why
     * sprinting automatically produces a faster stride for free. */
    if (f->grounded) {
        f->gait += speed * dt * 3.2f;
        if (f->gait > TAU_F) f->gait -= TAU_F;
    }

    f->anim_time += dt;
}

float fighter_attack_impact_time(const Fighter *f)
{
    if (!f->attacking) return 0.0f;

    const AttackDef *a = &ATTACKS[f->attack_index];
    return a->startup + a->active;
}

Fighter fighter_lerp(const Fighter *a, const Fighter *b, float t)
{
    Fighter r;
    r.x            = lerpf(a->x, b->x, t);
    r.y            = lerpf(a->y, b->y, t);
    r.z            = lerpf(a->z, b->z, t);
    r.vx           = lerpf(a->vx, b->vx, t);
    r.vy           = lerpf(a->vy, b->vy, t);
    r.vz           = lerpf(a->vz, b->vz, t);
    r.facing       = angle_lerp(a->facing, b->facing, t);
    /* gait wraps at TAU, so interpolate on the shortest arc as well */
    r.gait         = angle_lerp(a->gait, b->gait, t);
    r.anim_time    = lerpf(a->anim_time, b->anim_time, t);
    r.stamina      = lerpf(a->stamina, b->stamina, t);
    r.sprint_blend = lerpf(a->sprint_blend, b->sprint_blend, t);
    r.crouch_blend = lerpf(a->crouch_blend, b->crouch_blend, t);
    r.air_time     = lerpf(a->air_time, b->air_time, t);
    /* Landing resets this to its peak, so a naive lerp across the touchdown
     * tick would smear the squash on. Only ever ease it DOWNWARD. */
    r.land_blend   = (b->land_blend > a->land_blend)
                   ? b->land_blend
                   : lerpf(a->land_blend, b->land_blend, t);
    /* The swing is a smooth curve, so it interpolates like any other pose
     * value. Which swing it is, is not -- that snaps at the chain boundary. */
    r.attack_time   = lerpf(a->attack_time, b->attack_time, t);
    r.attack_strike = lerpf(a->attack_strike, b->attack_strike, t);
    r.regen_delay  = b->regen_delay;
    r.anim         = b->anim;
    r.grounded     = b->grounded;
    r.sprinting    = b->sprinting;
    r.exhausted    = b->exhausted;
    r.crouching       = b->crouching;
    r.attacking       = b->attacking;
    r.attack_index    = b->attack_index;
    r.attack_buffered = b->attack_buffered;
    r.attack_air      = b->attack_air;
    r.chain_grace     = b->chain_grace;
    r.struck          = b->struck;
    r.attack_aim      = b->attack_aim;
    r.alive           = b->alive;
    r.respawn         = b->respawn;
    /* Health steps down on one tick and drifts up on every other, so a plain
     * lerp is right for the drift and would smear the hit. Snap downward. */
    r.health          = (b->health < a->health)
                      ? b->health : lerpf(a->health, b->health, t);
    /* Likewise the flash: it is set to 1 on impact and decays. */
    r.hurt_flash      = (b->hurt_flash > a->hurt_flash)
                      ? b->hurt_flash : lerpf(a->hurt_flash, b->hurt_flash, t);
    return r;
}

/* ----------------------------- world ------------------------------------- */

/* ---------------------------------------------------------------------------
 * Deterministic randomness.
 *
 * xorshift32: four lines, no state beyond the word itself, and good enough for
 * damage rolls and aim error. Seeded by world_init with a fixed constant, never
 * from the clock -- see the note on World.rng for why that still satisfies
 * invariant 1.
 * ------------------------------------------------------------------------- */
uint32_t world_rand(World *w)
{
    uint32_t x = w->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    w->rng = x;
    return x;
}

int world_rand_range(World *w, int lo, int hi)
{
    if (hi <= lo) return lo;
    return lo + (int)(world_rand(w) % (uint32_t)(hi - lo + 1));
}

static float rand_unit(World *w)          /* -1 .. +1 */
{
    return (float)(world_rand(w) % 20001u) / 10000.0f - 1.0f;
}

/* ------------------------------- projectiles ----------------------------- */

static void bolt_spawn(World *w, const Fighter *from, int owner, float aim)
{
    for (int i = 0; i < BOLT_MAX; i++) {
        Bolt *b = &w->bolts[i];
        if (b->active) continue;
        b->active = true;
        b->x = from->x + sinf(aim) * 0.55f;
        b->y = from->y + BOLT_HEIGHT;
        b->z = from->z + cosf(aim) * 0.55f;
        b->vx = sinf(aim) * BOLT_SPEED;
        b->vz = cosf(aim) * BOLT_SPEED;
        b->life = BOLT_LIFE;
        b->owner = owner;
        return;
    }
    /* Pool full: the shot is simply lost. With BOLT_MAX at 24 and a 2.2s life
     * that needs both fighters swinging flat out, and dropping one is better
     * than growing the world struct mid-tick. */
}

static void hurt(World *w, Fighter *f)
{
    if (!f->alive) return;
    f->health -= (float)world_rand_range(w, DAMAGE_MIN, DAMAGE_MAX);
    f->hurt_flash = 1.0f;
    if (f->health <= 0.0f) {
        f->health = 0.0f;
        f->alive = false;
        f->respawn = RESPAWN_TIME;
        /* Drop everything that was in progress. Coming back mid-swing with a
         * buffered follow-up would be a gift nobody asked for. */
        f->attacking = false;
        f->attack_buffered = false;
        f->chain_grace = 0.0f;
        f->vx = f->vz = 0.0f;
    }
}

/* Does a bolt overlap this fighter? Measured against FIGHTER_RADIUS, never
 * against whatever mesh happens to be drawn -- invariant 7. */
static bool bolt_hits(const Bolt *b, const Fighter *f)
{
    if (!f->alive) return false;
    float dx = b->x - f->x;
    float dz = b->z - f->z;
    float r  = FIGHTER_RADIUS + BOLT_RADIUS;
    if (dx * dx + dz * dz > r * r) return false;
    /* Bolts fly at chest height, so a jump can carry you over one. */
    float dy = b->y - (f->y + FIGHTER_HEIGHT * 0.5f);
    return fabsf(dy) < FIGHTER_HEIGHT * 0.5f;
}

static void bolts_tick(World *w, float dt)
{
    for (int i = 0; i < BOLT_MAX; i++) {
        Bolt *b = &w->bolts[i];
        if (!b->active) continue;

        b->x += b->vx * dt;
        b->z += b->vz * dt;
        b->life -= dt;

        Fighter *target = (b->owner == SIDE_PLAYER) ? &w->enemy : &w->player;
        if (bolt_hits(b, target)) {
            hurt(w, target);
            b->active = false;
            continue;
        }

        float edge = ARENA_RADIUS + 4.0f;
        if (b->life <= 0.0f || b->x * b->x + b->z * b->z > edge * edge)
            b->active = false;
    }
}

/* A swing throws exactly one bolt, at the moment the strike lands, so the
 * projectile leaves on the same tick the animation reads as contact. */
static void fire_on_strike(World *w, Fighter *f, int side)
{
    if (!f->struck) return;

    /* The latched aim, NOT f->facing: fighter_tick turns a fighter toward
     * where it is travelling, so a strafing enemy would otherwise throw its
     * bolts out sideways. */
    bolt_spawn(w, f, side, f->attack_aim);
}

/* ----------------------------------- ai ---------------------------------- */
/*
 * Weak on purpose, in three separate ways, because any one of them alone reads
 * as a bug rather than as an opponent:
 *
 *   it thinks on a slow clock, so it commits to a bad idea for a while;
 *   it aims with an error rolled per shot and never leads a moving target;
 *   it picks its next mood from a weighted guess, not from what would work.
 */
#define AI_THINK_MIN     0.45f
#define AI_THINK_MAX     1.10f
#define AI_COOLDOWN      0.85f   /* between swings -- slower than a player   */
#define AI_AIM_ERROR     0.20f   /* radians, about 11 degrees                */
#define AI_RANGE_NEAR    5.0f
#define AI_RANGE_FAR    14.0f
#define AI_TURN_RATE     4.5f    /* radians/sec -- deliberately sluggish     */

static void ai_tick(World *w, Input *out, float dt)
{
    Ai *ai = &w->ai;
    Fighter *e = &w->enemy;
    const Fighter *p = &w->player;

    *out = (Input){ 0 };
    if (!e->alive) return;

    float dx = p->x - e->x;
    float dz = p->z - e->z;
    float dist = sqrtf(dx * dx + dz * dz);
    float to_player = atan2f(dx, dz);

    ai->think -= dt;
    ai->cooldown -= dt;

    if (ai->think <= 0.0f) {
        ai->think = AI_THINK_MIN
                  + (AI_THINK_MAX - AI_THINK_MIN)
                    * (float)(world_rand(w) % 1000u) / 1000.0f;
        ai->strafe = (world_rand(w) & 1u) ? 1.0f : -1.0f;

        /* Weighted, not optimal. A good fraction of the time it does the wrong
         * thing for the distance it is at, which is most of what makes it
         * beatable. */
        int roll = world_rand_range(w, 0, 99);
        if (dist > AI_RANGE_FAR)       ai->mood = (roll < 80) ? AI_CHASE : AI_WAIT;
        else if (dist > AI_RANGE_NEAR) ai->mood = (roll < 60) ? AI_FIGHT
                                                : (roll < 85) ? AI_CHASE : AI_WAIT;
        else                           ai->mood = (roll < 45) ? AI_FIGHT
                                                : (roll < 80) ? AI_BACK : AI_WAIT;
    }

    /* Face the player, slowly, and only ever toward where they are NOW. */
    float turn = to_player - e->facing;
    while (turn >  PI_F) turn -= TAU_F;
    while (turn < -PI_F) turn += TAU_F;
    float step = AI_TURN_RATE * dt;
    e->facing += (turn > step) ? step : (turn < -step ? -step : turn);

    float fx = sinf(to_player), fz = cosf(to_player);
    float sx = fz * ai->strafe, sz = -fx * ai->strafe;

    switch (ai->mood) {
    case AI_CHASE:
        out->move_x = fx;
        out->move_z = fz;
        break;
    case AI_FIGHT:
        /* Circle while shooting rather than standing still. */
        out->move_x = sx * 0.7f;
        out->move_z = sz * 0.7f;
        if (ai->cooldown <= 0.0f && !e->attacking) {
            out->attack = true;
            ai->cooldown = AI_COOLDOWN;
            /* Rolled once, here, and never corrected: it does not lead a
             * moving target, so crossing its line is how you make it miss. */
            ai->aim_error = rand_unit(w) * AI_AIM_ERROR;
            out->aim = to_player + ai->aim_error;
        }
        break;
    case AI_BACK:
        out->move_x = -fx;
        out->move_z = -fz;
        break;
    case AI_WAIT:
    default:
        break;
    }
}

/* ---------------------------------- world -------------------------------- */

static void life_tick(Fighter *f, float x, float z, float dt)
{
    if (f->alive) {
        f->health += HEALTH_REGEN * dt;
        if (f->health > HEALTH_MAX) f->health = HEALTH_MAX;
    } else {
        f->respawn -= dt;
        if (f->respawn <= 0.0f) {
            /* A clean slate rather than a patch-up: fighter_init already knows
             * every field, and a half-reset is how a stale flag survives. */
            fighter_init(f, x, z);
        }
    }
    f->hurt_flash -= dt * 3.0f;
    if (f->hurt_flash < 0.0f) f->hurt_flash = 0.0f;
}

void world_init(World *w)
{
    fighter_init(&w->player, 0.0f, 0.0f);
    fighter_init(&w->enemy, 0.0f, 9.0f);
    w->enemy.facing = PI_F;          /* looking back at the player */

    for (int i = 0; i < BOLT_MAX; i++) w->bolts[i] = (Bolt){ 0 };

    w->ai = (Ai){ .mood = AI_WAIT, .think = 0.6f, .cooldown = 0.5f,
                  .aim_error = 0.0f, .strafe = 1.0f };
    w->tick = 0;

    /* Fixed seed. Not the clock -- that is what would break determinism. */
    w->rng = 0x9E3779B9u;
}

void world_tick(World *w, Input in, const Cheats *cheats)
{
    /* Order is fixed: brain, then bodies, then what they threw. Everything
     * within a tick reads the same snapshot. */
    Input ai_in;
    ai_tick(w, &ai_in, TICK_DT);

    /* A dead fighter takes no orders. */
    if (!w->player.alive) in = (Input){ 0 };

    fighter_tick(&w->player, in, cheats);
    fighter_tick(&w->enemy, ai_in, cheats);

    fire_on_strike(w, &w->player, SIDE_PLAYER);
    fire_on_strike(w, &w->enemy, SIDE_ENEMY);

    bolts_tick(w, TICK_DT);

    life_tick(&w->player, 0.0f, 0.0f, TICK_DT);
    life_tick(&w->enemy, 0.0f, 9.0f, TICK_DT);

    w->tick++;
}
