# Arena Fighter

A small 3D arena fighter written in C17 with [raylib](https://www.raylib.com),
built from scratch as a readable example of how an action game actually fits
together: a fixed-rate deterministic simulation, an interpolating renderer, a
procedurally animated character, and an immediate-mode menu system.

No engine, no scene graph, no asset pipeline. The character is a hierarchy of
boxes posed with hand-written matrix math, which means every piece of it is
something you can read and change.

## Controls

| Input | Action |
| --- | --- |
| `WASD` | Move (camera-relative) |
| `Mouse` | Look |
| `Shift` | Sprint (drains stamina; running it to zero locks you out) |
| `Space` | Jump |
| `LMB` / `J` | Attack — press again mid-swing to chain jab → cross → finisher |
| `Tab` | Cheat menu (rebindable) |
| `Esc` | Pause menu → Settings / Keybinds / Main Menu |
| `Enter` | Start playing from the main menu |

Any key skips the intro.

## Building

Windows only for now. You need the **MSVC build tools** (Visual Studio 2022
Build Tools with the C++ workload) — raylib is already vendored, so there is
nothing else to install.

```
build.bat
```

That produces `ArenaFighter.exe` in the repo root. Object files stay in
`build/`. If `vcvars64.bat` lives somewhere other than the default path, edit
the one line at the top of `build.bat`.

## Running the tests

`src/fighter.c` deliberately depends on nothing but `game.h` and `math.h`, so
the entire simulation is testable without opening a window:

```
tests\run_tests.bat
```

## Project layout

```
src/
  main.c        window, input sampling, the fixed-timestep loop, screen routing
  game.h        shared types and tuning constants for the simulation
  fighter.c     all game logic: movement, stamina, jumping, the attack chain
  render.c      camera, the box-model character, arena, HUD
  ui.c / ui.h   a ~130-line immediate-mode UI (buttons, sliders, steppers)
  settings.c    options/keybind pages plus versioned, checksummed persistence
  frontend.c    the intro card and the main menu
  cheats.c      the debug cheat panel
vendor/         raylib 6.0 (headers + prebuilt MSVC binaries)
tests/          headless tests for the simulation
```

## How it works

A few ideas do most of the work, and they are worth knowing before changing
anything:

- **The simulation runs at a fixed 120 Hz**, decoupled from the frame rate. The
  renderer interpolates between the last two simulation states, so the game
  behaves identically at 30 FPS and 600 FPS. This is what keeps combat fair and
  what would make replays or rollback netcode possible later.
- **The simulation is deterministic.** Same inputs, same result, always. No
  randomness and no wall-clock time inside `world_tick`.
- **Animation is procedural.** There are no keyframes. Poses are computed from
  simulation state — the walk cycle advances by *distance travelled* rather than
  by time, which is why the legs never skate at any speed, and an entire attack
  swing is driven by a single scalar that eases from wind-up through impact to
  recovery.
- **The UI is immediate mode.** No widget objects, no retained tree; each frame
  asks "draw a button here, was it clicked?" and acts on the answer.

## Status

Working: an intro card that dissolves into a main menu over the live arena,
movement and camera, sprint/stamina with an exhaustion lockout, jumping, a
three-hit attack chain with input buffering, settings and rebindable keys with
persistence, and the cheat panel.

Not built yet: an enemy and its AI, damage and hit reactions, and the ability
system (the goal is four abilities per hero, some melee, some ranged).
`ANIM_HURT` and `ANIM_SPECIAL` exist in the animation enum waiting for those.

## Contributing

Contributions are welcome — see [CONTRIBUTING.md](CONTRIBUTING.md). `main` is
protected, so everything goes through a pull request.

## License

MIT — see [LICENSE](LICENSE).

This repo bundles raylib, which is zlib-licensed; see
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
