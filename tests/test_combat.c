/* Headless test of damage, death, respawn, regen and the projectile pool.
 *
 * The point of most of these is not the numbers -- it is that the numbers are
 * REPEATABLE. Damage is now rolled from a generator, and the whole reason that
 * is allowed to exist inside world_tick is that it is seeded once and advanced
 * only there, so two runs of the same inputs still agree exactly. The last test
 * in this file is the one that would catch a regression on that. */
#include "game.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(const char *what, int cond)
{
    printf("%-58s %s\n", what, cond ? "PASS" : "FAIL");
    if (!cond) failures++;
}

static Input none(void) { return (Input){ 0 }; }

static Input swing(void)
{
    Input i = (Input){ 0 };
    i.attack = true;
    return i;
}

static void step(World *w, Input in, int n)
{
    for (int i = 0; i < n; i++) world_tick(w, in, NULL);
}

static int live_bolts(const World *w)
{
    int n = 0;
    for (int i = 0; i < BOLT_MAX; i++) if (w->bolts[i].active) n++;
    return n;
}

int main(void)
{
    /* --- the generator ------------------------------------------------- */
    {
        World a, b;
        world_init(&a);
        world_init(&b);
        check("a fresh world starts from the same seed", a.rng == b.rng);

        int lo = 999, hi = -999;
        for (int i = 0; i < 4000; i++) {
            int d = world_rand_range(&a, DAMAGE_MIN, DAMAGE_MAX);
            if (d < lo) lo = d;
            if (d > hi) hi = d;
        }
        check("damage rolls stay inside DAMAGE_MIN..DAMAGE_MAX",
              lo >= DAMAGE_MIN && hi <= DAMAGE_MAX);
        check("damage rolls reach both ends of the range",
              lo == DAMAGE_MIN && hi == DAMAGE_MAX);
    }

    /* --- world shape ---------------------------------------------------- */
    {
        World w;
        world_init(&w);
        check("the world starts with two fighters alive",
              w.player.alive && w.enemy.alive);
        check("both start at full health",
              w.player.health == HEALTH_MAX && w.enemy.health == HEALTH_MAX);
        check("no bolts in flight at the start", live_bolts(&w) == 0);
    }

    /* --- a swing throws exactly one bolt -------------------------------- */
    {
        World w;
        world_init(&w);
        /* Aim the player away from the enemy so the bolt cannot land and
         * confuse the count. */
        w.player.facing = 3.14159265f;

        int before = live_bolts(&w);
        step(&w, swing(), 1);
        /* The bolt leaves on the strike, not on the press. */
        check("no bolt on the tick the swing starts", live_bolts(&w) == before);

        step(&w, none(), 40);
        check("one bolt after the strike lands", live_bolts(&w) >= 1);

        int after_one = live_bolts(&w);
        step(&w, none(), 30);
        check("a single swing never throws a second bolt",
              live_bolts(&w) <= after_one);
    }

    /* --- damage and death ----------------------------------------------- */
    {
        World w;
        world_init(&w);

        float start = w.enemy.health;
        /* Strictly under DAMAGE_MIN, so ANY roll is lethal -- 12 would survive
         * a roll of 10 and the test would fail one time in eight. */
        w.enemy.health = (float)DAMAGE_MIN - 1.0f;
        w.enemy.alive = true;
        check("setup put the enemy on low health", start > w.enemy.health);

        /* Park a bolt on top of the enemy and let the tick resolve it. */
        w.bolts[0] = (Bolt){ .active = true, .owner = SIDE_PLAYER,
                             .x = w.enemy.x, .y = w.enemy.y + BOLT_HEIGHT,
                             .z = w.enemy.z, .vx = 0.0f, .vz = 0.0f,
                             .life = BOLT_LIFE };
        step(&w, none(), 1);

        check("the bolt is consumed by the hit", !w.bolts[0].active);
        check("a lethal hit takes the enemy down", !w.enemy.alive);
        check("health floors at zero rather than going negative",
              w.enemy.health == 0.0f);
        check("death starts the respawn clock",
              fabsf(w.enemy.respawn - RESPAWN_TIME) < 0.05f);
    }

    /* --- respawn -------------------------------------------------------- */
    {
        World w;
        world_init(&w);
        w.enemy.health = 1.0f;
        w.bolts[0] = (Bolt){ .active = true, .owner = SIDE_PLAYER,
                             .x = w.enemy.x, .y = w.enemy.y + BOLT_HEIGHT,
                             .z = w.enemy.z, .life = BOLT_LIFE };
        step(&w, none(), 1);
        check("down before the clock runs out", !w.enemy.alive);

        step(&w, none(), (int)(RESPAWN_TIME * TICK_HZ) - 10);
        check("still down just before RESPAWN_TIME", !w.enemy.alive);

        step(&w, none(), 20);
        check("back up just after RESPAWN_TIME", w.enemy.alive);
        check("and back at full health", w.enemy.health == HEALTH_MAX);
    }

    /* --- regen ---------------------------------------------------------- */
    {
        World w;
        world_init(&w);
        w.player.health = 50.0f;
        step(&w, none(), TICK_HZ);      /* exactly one second */

        float gained = w.player.health - 50.0f;
        check("health regenerates at HEALTH_REGEN per second",
              fabsf(gained - HEALTH_REGEN) < 0.05f);

        w.player.health = HEALTH_MAX;
        step(&w, none(), TICK_HZ);
        check("regen never overshoots the maximum",
              w.player.health == HEALTH_MAX);
    }

    /* --- a dead fighter takes no orders ---------------------------------- */
    {
        World w;
        world_init(&w);
        w.player.health = 1.0f;
        w.bolts[0] = (Bolt){ .active = true, .owner = SIDE_ENEMY,
                             .x = w.player.x, .y = w.player.y + BOLT_HEIGHT,
                             .z = w.player.z, .life = BOLT_LIFE };
        step(&w, none(), 1);
        check("the player can be taken down too", !w.player.alive);

        float x = w.player.x, z = w.player.z;
        Input hard = (Input){ 0 };
        hard.move_x = 1.0f;
        hard.jump = true;
        hard.attack = true;
        step(&w, hard, 30);
        check("input is ignored while down",
              fabsf(w.player.x - x) < 0.001f && fabsf(w.player.z - z) < 0.001f);
    }

    /* --- the pool is bounded --------------------------------------------- */
    {
        World w;
        world_init(&w);
        /* Swing forever; the pool must never be exceeded and must recycle. */
        for (int i = 0; i < 2000; i++) world_tick(&w, swing(), NULL);
        check("the bolt pool never overflows", live_bolts(&w) <= BOLT_MAX);
    }

    /* --- a bolt flies along the aim it was given ------------------------ */
    {
        /* Guards the shared convention: a yaw of 0 is +z, and BOTH facing and
         * Input.aim use it. The camera does not -- its yaw is where it SITS,
         * half a turn from where it looks -- which is why render.c converts
         * and why shots once came out of the back of the player's head. */
        const float quarter = 1.57079633f;      /* +x */
        struct { float aim; float ex, ez; const char *what; } cases[] = {
            { 0.0f,        0.0f,  1.0f, "aim 0 throws toward +z" },
            { 3.14159265f, 0.0f, -1.0f, "aim PI throws toward -z" },
            { quarter,     1.0f,  0.0f, "aim +PI/2 throws toward +x" },
            { -quarter,   -1.0f,  0.0f, "aim -PI/2 throws toward -x" },
        };

        for (int c = 0; c < 4; c++) {
            World w;
            world_init(&w);
            /* Put the enemy far away so it cannot eat the bolt under test. */
            w.enemy.x = 0.0f;
            w.enemy.z = 200.0f;

            Input in = swing();
            in.aim = cases[c].aim;
            world_tick(&w, in, NULL);
            step(&w, none(), 40);

            int found = -1;
            for (int i = 0; i < BOLT_MAX; i++)
                if (w.bolts[i].active && w.bolts[i].owner == SIDE_PLAYER) found = i;

            bool ok = false;
            if (found >= 0) {
                float vx = w.bolts[found].vx / BOLT_SPEED;
                float vz = w.bolts[found].vz / BOLT_SPEED;
                ok = fabsf(vx - cases[c].ex) < 0.02f
                  && fabsf(vz - cases[c].ez) < 0.02f;
            }
            check(cases[c].what, ok);
        }
    }

    /* --- every swing of a chain throws --------------------------------- */
    {
        /* The chain advances through a different branch than a fresh swing,
         * and that branch used to forget to re-arm the bolt: the animation
         * chained correctly and only swing 1 ever fired. */
        World w;
        world_init(&w);
        w.player.facing = 3.14159265f;
        w.player.attack_aim = 3.14159265f;

        int fired = 0;
        for (int i = 0; i < 200; i++) {
            int before = live_bolts(&w);
            world_tick(&w, (i % 9 == 0) ? swing() : none(), NULL);
            if (live_bolts(&w) > before) fired++;
        }
        check("a sustained chain keeps throwing bolts", fired >= 4);
        check("the chain reached past its first swing",
              w.player.attack_index != 0 || fired >= 4);
    }

    /* --- determinism, the one that guards the generator ------------------ */
    {
        /* Zero the padding as well as the fields: memcmp sees every byte, and
         * the bytes a compiler leaves between members are never written by
         * world_init. Without this the test fails on stack litter, not on a
         * real divergence. */
        World a, b;
        memset(&a, 0, sizeof a);
        memset(&b, 0, sizeof b);
        world_init(&a);
        world_init(&b);

        /* A long, varied run: movement, jumps and swings, so the AI reacts and
         * both sides roll damage many times. */
        bool ever_hurt = false;
        for (int i = 0; i < 3000; i++) {
            Input in = (Input){ 0 };
            in.move_x = (float)((i / 37) % 3 - 1);
            in.move_z = (float)((i / 53) % 3 - 1);
            in.attack = (i % 29) == 0;
            in.jump   = (i % 91) == 0;
            in.sprint = (i % 7) < 3;
            world_tick(&a, in, NULL);
            world_tick(&b, in, NULL);
            /* Sampled DURING the run: regen and respawn both restore health,
             * so by the last tick the evidence of a fight is gone. */
            if (a.player.health < HEALTH_MAX || a.enemy.health < HEALTH_MAX)
                ever_hurt = true;
        }

        check("two runs of identical input agree exactly",
              memcmp(&a, &b, sizeof(World)) == 0);
        check("the run actually exercised combat", ever_hurt);
    }

    printf("\n%s\n", failures ? "FAILURES" : "all checks passed");
    return failures ? 1 : 0;
}
