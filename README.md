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
| `Ctrl` | Crouch — slower, but a lower profile (rebindable) |
| `Space` | Jump — costs stamina from the same bar sprinting uses |
| `LMB` / `J` | Attack — press again mid-swing to chain jab → cross → finisher. Works in the air |
| `Tab` | Cheat menu (rebindable) |
| `Esc` | Pause menu → Settings / Keybinds / Main Menu |
| `Enter` | Start playing from the main menu |

Any key skips the intro.

## Character skins

The game draws its own box fighter and ships with no art. If you want something
better, put a rigged `.glb` or `.gltf` in an `assets/` folder next to the
executable and pick it under **Customize → Mod Skins** in the main menu. The
fighter standing in the arena behind the panel is the live preview: drag
anywhere outside the panel to turn him around, scroll to zoom.

Any model is scaled to the right height and stood on the floor automatically,
and its animations are matched to the game's states by name (`idle`, `walk`,
`run`, `jump`, `punch`, …), including the Source 2 naming that Dota heroes use,
where `run` stands in for a walk that does not exist. The Mod Skins page tells
you how many clips a file has and how many states matched, so a bad export is
visible rather than mysterious. The simulation still drives them: movement
clips advance with ground speed — their authored rate at walking pace, so a run
cycle looks the way its source game played it and the feet don't skate — and an
attack clip plays at its authored rate, anchored so its contact frame lands on
the game's hit window. With no `assets/` folder the built-in fighter is used,
so nothing here is required.

Models authored Z-up (anything out of Source 2) are detected and stood upright
automatically. If a skin faces the wrong way, flip `MODEL_YAW` in
`src/model.c`. A skin exported as `.glb` plus loose `.png` textures needs the
textures kept alongside it in `assets/`, or it will load untextured.

**Export only the animations you need.** Load time is driven by the *number of
clips* in the file, not its size — raylib resamples every one of them at load,
used or not. A character exported with its full animation set can carry
hundreds; asking the exporter for the handful this game uses (`idle`, a walk or
run, an attack, a death) takes a 31 MB / 1.8s load down to 1.4 MB / 0.3s.

**A skin is a costume, not a stat.** Every fighter is the same height whatever
model is drawn — imported ones are scaled to it on load — and there is no size
option, because being drawn bigger or smaller would change how far you reach and
how big a target you make.

Good sources of models that are free to use and to redistribute:

- [Kenney](https://kenney.nl) — CC0, and the modular *Blocky Characters* pack
  suits this game's look particularly well
- [KayKit](https://kaylousberg.com) — CC0 rigged character packs
- [Quaternius](https://quaternius.com) — CC0 characters and animation libraries

**`assets/` is deliberately not tracked by git.** Model files belong to whoever
made them, and this repository is public and MIT licensed — committing one would
be redistributing it under a licence we have no right to grant. Keep your models
local.

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
  frontend.c    the intro card, the main menu, customize and mod skins
  model.c       optional imported glTF skins: loading, clip choice, playback
  hero.c        who you play as: skin and colours
  cheats.c      the debug cheat panel
vendor/         raylib 6.0 (headers + prebuilt MSVC binaries)
tests/          headless tests: the attack chain, and movement/stamina/crouch
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
  recovery. Jumping, landing and crouching work the same way: the simulation
  names the phase, and the pose is blended from continuous values so the states
  flow into each other instead of snapping.
- **The UI is immediate mode.** No widget objects, no retained tree; each frame
  asks "draw a button here, was it clicked?" and acts on the answer.

## Status

Working: an intro card that dissolves into a main menu over the live arena,
movement and camera, sprint and jumping paid for out of one stamina bar with an
exhaustion lockout, crouching, a three-hit attack chain with input buffering
that also works in the air, a procedural pose for every movement state, settings
and rebindable keys with persistence, optional imported character skins with colour options and an
inspection camera, and the cheat panel.

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
