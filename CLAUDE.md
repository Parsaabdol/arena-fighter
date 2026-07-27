# Working on this repo

**Read [ARCHITECTURE.md](ARCHITECTURE.md) first.** It is the high-level map of
the system: the frame flow, the module boundaries, the invariants, and an
honest account of what is built versus stubbed.

Keep it accurate. Any change that adds a subsystem, alters the frame flow,
touches an invariant, or moves something from "stubbed" to "working" updates
ARCHITECTURE.md in the same commit — including its "Last updated" line.

## Non-negotiables

These are restated from ARCHITECTURE.md §4 because they are the ones that get
broken by accident:

1. No randomness and no wall-clock time reachable from `world_tick` — the
   simulation is deterministic.
2. Game logic uses `TICK_DT`, never `GetFrameTime()`.
3. `render.c` never mutates simulation state.
4. Every new `Fighter` field must be handled in `fighter_lerp`.
5. `src/fighter.c` stays free of raylib, so the simulation remains testable
   headlessly.
6. Save-format changes bump `SAVE_VERSION` in `settings.c`.

## Build and test

```
build.bat            -> ArenaFighter.exe in the repo root
tests\run_tests.bat  -> headless simulation tests
```

The project compiles at `/W4` with zero warnings. Keep it that way.

## Repo conventions

- `main` is protected: pull request required, no force pushes, no deletions.
- Match the surrounding code style. Comments explain *why*, not what.
- Tuning values get a named `#define`, not an inline literal.
