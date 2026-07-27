/* Headless test of jumping, stamina and crouching. Like the attack tests, this
 * runs the real simulation with no window: fighter.c depends on nothing but
 * game.h and math.h. */
#include "game.h"

#include <math.h>
#include <stdio.h>

static int failures = 0;

static void check(const char *what, int cond)
{
    printf("%-58s %s\n", what, cond ? "PASS" : "FAIL");
    if (!cond) failures++;
}

static Input none(void) { return (Input){ 0 }; }

static Input run(void)
{
    Input i = (Input){ 0 };
    i.move_z = 1.0f;
    i.sprint = true;
    return i;
}

static Input walk(void)     { Input i = (Input){ 0 }; i.move_z = 1.0f; return i; }
static Input crouch_walk(void)
{
    Input i = walk();
    i.crouch = true;
    return i;
}

static void idle(Fighter *f, int n)
{
    for (int i = 0; i < n; i++) fighter_tick(f, none(), NULL);
}

/* Hold `in` until the fighter is back on the ground, so a test can measure a
 * whole jump without hardcoding its length. */
static int fly(Fighter *f, Input in, int limit)
{
    int n = 0;
    while (!f->grounded && n < limit) { fighter_tick(f, in, NULL); n++; }
    return n;
}

int main(void)
{
    /* --- a jump costs stamina ------------------------------------------- */
    {
        Fighter f; fighter_init(&f, 0, 0);
        float before = f.stamina;

        Input jump = none(); jump.jump = true;
        fighter_tick(&f, jump, NULL);

        check("jumping leaves the ground", !f.grounded);
        printf("   stamina: %.1f -> %.1f  (cost %.0f)\n",
               (double)before, (double)f.stamina, (double)JUMP_STAMINA_COST);
        check("the jump was paid for out of the stamina bar",
              f.stamina <= before - JUMP_STAMINA_COST + 0.001f);
    }

    /* --- you cannot jump forever ---------------------------------------- */
    {
        Fighter f; fighter_init(&f, 0, 0);
        Input jump = none(); jump.jump = true;

        int jumps = 0;
        for (int attempt = 0; attempt < 12; attempt++) {
            if (!f.grounded) break;
            float before = f.stamina;
            fighter_tick(&f, jump, NULL);
            if (!f.grounded) {
                jumps++;
                fly(&f, none(), 400);        /* ride it out, no regen delay skip */
            } else {
                /* Refused: the bar could not cover it. */
                check("a refused jump costs nothing", f.stamina >= before - 0.001f);
                break;
            }
        }
        printf("   jumps before the bar said no: %d\n", jumps);
        check("jumping back to back runs the bar out", jumps > 0 && jumps < 12);
        check("the fighter is on the ground when it runs out", f.grounded);
    }

    /* --- exhaustion grounds you completely ------------------------------- */
    {
        Fighter f; fighter_init(&f, 0, 0);

        /* Sprint the bar to zero. */
        int n = 0;
        while (!f.exhausted && n < 2000) { fighter_tick(&f, run(), NULL); n++; }
        check("sprinting to zero triggers the exhaustion lockout", f.exhausted);

        Input jump = run(); jump.jump = true;
        fighter_tick(&f, jump, NULL);
        check("exhausted cannot jump at all", f.grounded);
    }

    /* --- landing sets the squash, and it decays -------------------------- */
    {
        Fighter f; fighter_init(&f, 0, 0);
        Input jump = none(); jump.jump = true;
        fighter_tick(&f, jump, NULL);

        check("no squash while airborne", f.land_blend == 0.0f);
        fly(&f, none(), 400);
        check("landing squashes", f.land_blend > 0.1f);
        check("the landing is named", f.anim == ANIM_LAND);

        idle(&f, 40);
        check("the squash settles back out", f.land_blend == 0.0f);
    }

    /* --- the jump is named through its phases ---------------------------- */
    {
        Fighter f; fighter_init(&f, 0, 0);
        Input jump = none(); jump.jump = true;
        fighter_tick(&f, jump, NULL);
        check("takeoff first", f.anim == ANIM_JUMP_TAKEOFF);

        int saw_rise = 0, saw_apex = 0, saw_fall = 0;
        while (!f.grounded) {
            fighter_tick(&f, none(), NULL);
            if (f.anim == ANIM_JUMP_RISE) saw_rise = 1;
            if (f.anim == ANIM_JUMP_APEX) saw_apex = 1;
            if (f.anim == ANIM_JUMP_FALL) saw_fall = 1;
        }
        check("then rise, apex and fall", saw_rise && saw_apex && saw_fall);
    }

    /* --- crouching is slower than walking -------------------------------- */
    {
        Fighter a, b;
        fighter_init(&a, 0, 0);
        fighter_init(&b, 0, 0);

        for (int i = 0; i < 120; i++) {          /* a second, well past top speed */
            fighter_tick(&a, walk(), NULL);
            fighter_tick(&b, crouch_walk(), NULL);
        }
        float wv = sqrtf(a.vx * a.vx + a.vz * a.vz);
        float cv = sqrtf(b.vx * b.vx + b.vz * b.vz);
        printf("   top speed: walk %.2f  crouch %.2f\n", (double)wv, (double)cv);

        check("crouching is slower than walking", cv < wv * 0.75f);
        check("crouching still moves you", cv > 0.5f);
        check("crouching is named", b.anim == ANIM_CROUCH);
    }

    /* --- crouch rules ---------------------------------------------------- */
    {
        Fighter f; fighter_init(&f, 0, 0);

        Input cr = crouch_walk(); cr.sprint = true;
        for (int i = 0; i < 30; i++) fighter_tick(&f, cr, NULL);
        check("you cannot sprint while crouched", !f.sprinting);

        /* Crouch is grounded-only, so a held crouch is dropped on takeoff and
         * picked back up one tick after landing -- the crouch decision reads
         * the ground state from before gravity resolves contact. */
        Input cj = crouch_walk(); cj.jump = true;
        fighter_tick(&f, cj, NULL);
        check("you can jump out of a crouch", !f.grounded);
        check("crouch is released in the air", !f.crouching);

        fly(&f, crouch_walk(), 400);
        check("landed still holding crouch", f.grounded && !f.crouching);
        fighter_tick(&f, crouch_walk(), NULL);
        check("crouch is back one tick after landing", f.crouching);
    }

    /* --- determinism still holds with the new inputs --------------------- */
    {
        Fighter a, b;
        fighter_init(&a, 0, 0);
        fighter_init(&b, 0, 0);
        for (int i = 0; i < 600; i++) {
            Input in = (Input){ 0 };
            in.move_x = (i % 3) ? 1.0f : 0.0f;
            in.move_z = (i % 5) ? -1.0f : 0.0f;
            in.jump   = (i % 41 == 0);
            in.attack = (i % 17 == 0);
            in.crouch = ((i / 30) % 2) == 0;
            in.sprint = (i % 7) != 0;
            fighter_tick(&a, in, NULL);
            fighter_tick(&b, in, NULL);
        }
        check("two runs of identical input agree exactly",
              a.x == b.x && a.z == b.z && a.y == b.y
              && a.stamina == b.stamina && a.anim == b.anim);
    }

    printf("\n%s\n", failures ? "SOME CHECKS FAILED" : "all checks passed");
    return failures != 0;
}
