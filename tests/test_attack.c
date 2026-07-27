/* Headless test of the attack chain. fighter.c depends on nothing but game.h
 * and math.h, so the whole combat state machine is testable without a window. */
#include "game.h"

#include <math.h>
#include <stdio.h>

static int failures = 0;

static void check(const char *what, int cond)
{
    printf("%-58s %s\n", what, cond ? "PASS" : "FAIL");
    if (!cond) failures++;
}

static Input none(void)      { return (Input){ 0 }; }
static Input press_attack(void) { Input i = (Input){ 0 }; i.attack = true; return i; }

/* Run n ticks of nothing. */
static void idle(Fighter *f, int n)
{
    for (int i = 0; i < n; i++) fighter_tick(f, none(), NULL);
}

int main(void)
{
    /* --- a single press plays swing 1 and ends -------------------------- */
    {
        Fighter f; fighter_init(&f, 0, 0);
        check("idle: not attacking", !f.attacking);

        fighter_tick(&f, press_attack(), NULL);
        check("press starts a swing", f.attacking);
        check("first swing of the chain is index 0", f.attack_index == 0);
        check("anim is ANIM_ATTACK", f.anim == ANIM_ATTACK);

        /* swing 1 total = 0.09 + 0.07 + 0.17 = 0.33s -> 40 ticks at 120Hz */
        idle(&f, 45);
        check("swing ends on its own", !f.attacking);
        check("strike returns to rest", fabsf(f.attack_strike) < 0.0001f);
        check("chain stays continuable briefly", f.chain_grace > 0.0f);
    }

    /* --- the strike curve is bounded and shaped ------------------------- */
    {
        Fighter f; fighter_init(&f, 0, 0);
        fighter_tick(&f, press_attack(), NULL);

        float lo = 0.0f, hi = 0.0f;
        int peak_tick = -1, t = 0;
        while (f.attacking) {
            if (f.attack_strike < lo) lo = f.attack_strike;
            if (f.attack_strike > hi) { hi = f.attack_strike; peak_tick = t; }
            fighter_tick(&f, none(), NULL);
            t++;
        }
        printf("   curve: min %.3f  max %.3f  peak at tick %d of %d\n",
               (double)lo, (double)hi, peak_tick, t);
        check("winds up before striking (curve goes negative)", lo < -0.3f);
        check("reaches full extension at impact", hi > 0.98f && hi <= 1.0001f);
        /* Recovery is the longest phase by design -- you are meant to be
         * vulnerable after swinging -- so impact sits around the midpoint,
         * never at the very end. */
        check("impact lands mid-swing with a real recovery tail after it",
              peak_tick > t / 4 && peak_tick <= t / 2 && (t - peak_tick) >= 8);
    }

    /* --- pressing during recovery chains 0 -> 1 -> 2 -> 0 --------------- */
    {
        Fighter f; fighter_init(&f, 0, 0);
        int seen[8], n = 0;

        for (int swing = 0; swing < 4; swing++) {
            fighter_tick(&f, press_attack(), NULL);
            seen[n++] = f.attack_index;
            /* Sit in the combo window. 36 ticks (0.30s) is past startup+active
             * for all three swings -- including the finisher at 0.29s -- and
             * short of the shortest total (0.33s), so it lands in recovery
             * every time. */
            idle(&f, 36);
            check("still mid-swing inside the combo window", f.attacking);
        }
        printf("   chain order: %d %d %d %d\n", seen[0], seen[1], seen[2], seen[3]);
        check("chain advances 0,1,2 then wraps to 0",
              seen[0] == 0 && seen[1] == 1 && seen[2] == 2 && seen[3] == 0);
    }

    /* --- input buffering: a press during startup still chains ----------- */
    {
        Fighter f; fighter_init(&f, 0, 0);
        fighter_tick(&f, press_attack(), NULL);
        /* press again 2 ticks in -- far too early to chain */
        fighter_tick(&f, press_attack(), NULL);
        fighter_tick(&f, press_attack(), NULL);
        check("early press is buffered, not dropped", f.attack_buffered || f.attack_index == 1);
        idle(&f, 25);
        check("buffered press chains to swing 2 when the window opens",
              f.attack_index == 1 && f.attacking);
    }

    /* --- letting the chain lapse restarts at swing 1 -------------------- */
    {
        Fighter f; fighter_init(&f, 0, 0);
        fighter_tick(&f, press_attack(), NULL);
        idle(&f, 22);
        fighter_tick(&f, press_attack(), NULL);
        check("chained to swing 2", f.attack_index == 1);

        idle(&f, 200);                       /* well past grace */
        check("chain lapsed back to swing 1", f.attack_index == 0);
        fighter_tick(&f, press_attack(), NULL);
        check("fresh press starts the chain over", f.attack_index == 0);
    }

    /* --- commitment rules ----------------------------------------------- */
    {
        Fighter f; fighter_init(&f, 0, 0);
        fighter_tick(&f, press_attack(), NULL);

        Input jump = (Input){ 0 }; jump.jump = true;
        fighter_tick(&f, jump, NULL);
        check("cannot jump out of an attack", f.grounded);

        Input sprint = (Input){ 0 };
        sprint.sprint = true; sprint.move_z = 1.0f;
        fighter_tick(&f, sprint, NULL);
        check("cannot sprint while attacking", !f.sprinting);
    }

    /* --- attacking lunges forward --------------------------------------- */
    {
        Fighter f; fighter_init(&f, 0, 0);
        f.facing = 0.0f;                     /* facing +z */
        float z0 = f.z;
        fighter_tick(&f, press_attack(), NULL);
        idle(&f, 25);
        printf("   lunge: z %.3f -> %.3f\n", (double)z0, (double)f.z);
        check("the swing steps forward", f.z > z0 + 0.02f);
    }

    /* --- airborne cannot start an attack -------------------------------- */
    {
        Fighter f; fighter_init(&f, 0, 0);
        Input jump = (Input){ 0 }; jump.jump = true;
        fighter_tick(&f, jump, NULL);
        check("airborne", !f.grounded);
        fighter_tick(&f, press_attack(), NULL);
        check("no attack starts in the air", !f.attacking);
    }

    /* --- determinism: same inputs, same result -------------------------- */
    {
        Fighter a, b;
        fighter_init(&a, 0, 0);
        fighter_init(&b, 0, 0);
        for (int i = 0; i < 300; i++) {
            Input in = (i % 17 == 0) ? press_attack() : none();
            in.move_x = (i % 3) ? 1.0f : 0.0f;
            fighter_tick(&a, in, NULL);
            fighter_tick(&b, in, NULL);
        }
        check("two runs of identical input agree exactly",
              a.x == b.x && a.z == b.z && a.attack_index == b.attack_index
              && a.attack_time == b.attack_time);
    }

    printf("\n%s\n", failures ? "SOME CHECKS FAILED" : "all checks passed");
    return failures != 0;
}
