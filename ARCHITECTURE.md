# Architecture & current state

A high-level map of Arena Fighter: how the pieces fit, which rules must not be
broken, and what is actually built versus merely sketched.

> **Keep this current.** Any change that adds a subsystem, changes the frame
> flow, breaks or adds an invariant, or moves something from "stubbed" to
> "working" should update this file in the same commit. It is meant to be
> loadable as the single piece of context that explains the project.
>
> Last updated: 2026-07-26 — after the attack chain and the menu split.

---

## 1. What this is

A 3D arena fighter in C17 on raylib, with no engine underneath it. Roughly 1,700
lines across seven source files. The character is a hierarchy of boxes posed by
hand-written matrix math; the UI is immediate mode; the simulation is a fixed
120 Hz deterministic tick that the renderer interpolates between.

The design goal is legibility: every subsystem should be something you can read
end to end in one sitting.

---

## 2. The frame

This is the mental model everything else hangs off. One iteration of the loop in
`main.c`:

```
   poll input  ──►  Escape / cheat key handled first (menu routing)
                    │
                    ├─ paused?  ─── yes ─►  freeze: prev = curr, accumulator = 0
                    │                       release the cursor
                    │
                    └─ no ──►  read mouse delta ──► render_camera_look()
                               latch jump / attack presses
                               accumulator += min(frameTime, MAX_FRAME_TIME)
                               │
                               while accumulator >= TICK_DT:
                                   prev = curr
                                   world_tick(&curr, in, &cheats)   ← all game logic
                                   consume the one-shot inputs
                                   accumulator -= TICK_DT
                    │
   alpha = accumulator / TICK_DT      ← how far between prev and curr we landed
                    │
   render_begin()
     render_world(prev, curr, alpha)  ← interpolates, never mutates
     render_hud(curr, alpha, ...)
     menu overlay for the current Screen (pause / settings / keybinds / cheats)
   render_end()
```

Two consequences worth internalising:

- **Some frames run zero ticks** (at 600 FPS with a 120 Hz sim, most do). This is
  why one-shot inputs like jump and attack are *latched* in the frame loop and
  consumed in the tick loop — sampling them per-frame would silently drop
  presses.
- **Some frames run several ticks** (after a stall). `MAX_FRAME_TIME` caps the
  catch-up so a hitch can't spiral into an unbounded tick loop.

---

## 3. Module map

| File | Owns | May depend on |
| --- | --- | --- |
| `main.c` | Window, input sampling, the fixed-timestep loop, menu routing, settings commit | everything |
| `game.h` | Shared types (`World`, `Fighter`, `Input`, `Cheats`) and cross-cutting tuning constants | — |
| `fighter.c` | **All** game logic: movement, stamina, jump, attack chain, animation state | `game.h`, `math.h` only |
| `render.c` | Camera, character model, arena, HUD | `game.h`, raylib |
| `ui.c` / `ui.h` | Immediate-mode widgets: button, slider, cycler, text | raylib |
| `settings.c` / `.h` | Option/keybind pages, applying settings to the window, persistence | `ui.h`, raylib |
| `cheats.c` / `.h` | The cheat overlay | `ui.h`, raylib, `game.h` |
| `tests/` | Headless simulation tests | `game.h`, `fighter.c` |

**`fighter.c` deliberately has no raylib dependency.** That is what makes the
entire combat system testable from a console in under a second, and it is worth
protecting.

---

## 4. Invariants

Breaking one of these is how this codebase would rot. They are load-bearing:

1. **The simulation is deterministic.** No randomness, no wall-clock time
   anywhere reachable from `world_tick`. Same inputs → same result, always. This
   is what keeps replays and rollback netcode possible later, and it is why the
   attack chain is a fixed order rather than a random pick.
2. **Game logic runs on `TICK_DT`, never `GetFrameTime()`.** Anything reading the
   real clock lives in `main.c` or `render.c`.
3. **The renderer never mutates simulation state.** `render.c` takes `const`
   world state. The camera lives in `render.c` precisely so it cannot influence
   gameplay — `main.c` reads its yaw back to make movement camera-relative, and
   that is the only direction information flows.
4. **Every `Fighter` field must be handled in `fighter_lerp`.** It builds a fresh
   struct field by field; a forgotten field is uninitialised memory read by the
   renderer. This is the single easiest mistake to make here.
5. **Simulation state is plain old data.** `World` and `Fighter` are copyable
   with `=`; the loop relies on that for `prev = curr`. No pointers, no
   ownership, no allocation in the tick.
6. **Save-format changes bump `SAVE_VERSION`.** An old file is then rejected
   rather than misread.

---

## 5. Data model

```
World
 ├─ Fighter player
 └─ uint64 tick

Fighter                        (all POD, all interpolated in fighter_lerp)
 ├─ transform     x/y/z, vx/vy/vz, facing
 ├─ locomotion    grounded, gait, sprint_blend
 ├─ stamina       stamina, sprinting, exhausted, regen_delay
 ├─ attack        attacking, attack_index, attack_time,
 │                attack_strike, attack_buffered, chain_grace
 ├─ animation     anim (AnimState), anim_time
 └─ health        (present, never damaged yet)

Input     move_x/move_z (already world-space), attack*, special, jump*, sprint
          * edge-triggered: true only on the tick that acts
Cheats    session-only, never persisted
Settings  fps/res/display/sensitivity indices + vsync + cheat_key
```

`Input.move_*` arrives in **world space** — `main.c` rotates the raw key
directions by camera yaw before handing them over, so the simulation never needs
to know a camera exists.

---

## 6. Subsystem state

| Subsystem | State | Notes |
| --- | --- | --- |
| Fixed-tick loop + interpolation | ✅ working | 120 Hz, `MAX_FRAME_TIME` guard |
| Movement, accel/friction, facing | ✅ working | camera-relative, diagonal-normalised |
| Jump + gravity + air control | ✅ working | momentum carries through a jump |
| Sprint + stamina + exhaustion lockout | ✅ working | lockout is what makes it a decision |
| Arena bounds | ✅ working | circular, slides along the rim |
| Attack chain (3 swings) | ✅ working | see §7 |
| Procedural animation | ✅ working | gait by distance; attack pose from one scalar |
| Third-person orbit camera | ✅ working | smoothed, pitch-clamped |
| Immediate-mode UI | ✅ working | button / slider / cycler |
| Pause → Settings / Keybinds pages | ✅ working | Escape opens pause, not options |
| Settings persistence | ✅ working | versioned + checksummed, beside the exe |
| Cheat panel | ✅ working | unlimited sprint only |
| Headless tests | ✅ working | 24 checks over the attack chain |
| **Enemy + AI** | ❌ absent | `World` holds only `player` |
| **Damage / hitboxes / hit reactions** | ❌ absent | `health` exists, nothing writes it |
| **Abilities (goal: 4 per hero, melee + ranged)** | ❌ absent | `Input.special` sampled, unused |

---

## 7. The attack chain

The most recently built system, and the template for abilities.

Three swings chained in **fixed order** — jab → cross → finisher — never random.
Each has the standard three phases:

```
  STARTUP        ACTIVE          RECOVERY
  wind up        hit window      vulnerable + combo window
  ├──────────────┼───────────────┼─────────────────────────►
                 ▲               ▲
                 hitbox goes     press attack in here to
                 here (todo)     advance the chain
```

| Swing | startup | active | recovery | lunge |
| --- | --- | --- | --- | --- |
| jab | 0.09s | 0.07s | 0.17s | 26 |
| cross | 0.11s | 0.08s | 0.21s | 32 |
| finisher | 0.19s | 0.10s | 0.36s | 46 |

**One scalar drives the whole animation.** `attack_strike` eases negative during
wind-up, accelerates to `1.0` at impact, holds there briefly (so the contact pose
is actually drawn, and so it reads as a hit), then eases back to 0. `render.c`
multiplies that curve by per-swing amplitudes — which is why three distinct
attacks cost one block of code, and a fourth costs about six lines.

Supporting behaviour: a press arriving too early is **buffered** and spent when
the window opens; a **0.22s grace** after the swing keeps the chain alive so a
natural rhythm still reaches swings 2 and 3; attacking cuts speed to 40%,
acceleration to 30%, turn rate to 30%, and blocks jump and sprint.

---

## 8. Where to change what

| I want to… | Go to |
| --- | --- |
| Retune movement/combat feel | constants at the top of `fighter.c`; shared ones in `game.h` |
| Add a 4th attack | `ATTACKS[]` in `fighter.c`, `ATTACK_CHAIN` in `game.h`, pose `switch` in `render.c` |
| Add an ability | new state on `Fighter`, drive from `Input.special`, mirror the attack-chain shape |
| Add an enemy | second `Fighter` in `World`; `world_tick` already owns the ordering |
| Add damage | the ACTIVE window in `fighter_tick`; then `ANIM_HURT`, then a health bar in `render.c` |
| Change the character model | `draw_fighter` / `draw_limb` in `render.c` |
| Add a setting | `Settings` struct, the record in `settings.c` (bump `SAVE_VERSION`), a row in `settings_menu` |
| Add a widget | `ui.c` — keep it stateless apart from the drag-capture pointer |
| Add a test | `tests/test_attack.c`, or a sibling file wired into `run_tests.bat` |

---

## 9. Known rough edges

Small, deliberate, and worth knowing before they surprise you:

- **`MENU_REVERT` is dead.** `main.c` still handles it, but no menu emits it any
  more since the Revert button became Restore Defaults. Harmless; remove when
  convenient.
- **`ANIM_HURT` and `ANIM_SPECIAL`** are declared and never set — placeholders
  for damage and abilities.
- **`Input.special`** is sampled every frame and read by nothing.
- **`Fighter.health`** is initialised and interpolated but never reduced.
- **Attacks are ground-only** and cannot be started or continued in the air.
- **Settings live beside the executable**, so moving the exe leaves its config
  behind. Deliberate (portable), but it is why a stray second exe used to look
  like it had "lost" your settings.
- **Windows/MSVC only.** `build.bat` hardcodes the `vcvars64.bat` path.

---

## 10. Roadmap

In dependency order — each unblocks the next:

1. **Damage + hitboxes.** The ACTIVE window already exists; give it a shape, a
   damage number, and `ANIM_HURT`. Needs a target, so realistically lands with 2.
2. **Enemy fighter + AI.** A second `Fighter` in `World` reusing `fighter_tick`,
   plus a small approach/attack/retreat state machine. Must stay deterministic.
3. **Ability system.** Goal is four per hero, some melee, some ranged. Ranged
   implies projectiles as simulation entities, which is the first real change to
   the `World` shape.
