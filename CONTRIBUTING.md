# Contributing to Arena Fighter

Thanks for taking an interest. This is a small project with a single
maintainer, so the process is deliberately light — but there is one hard rule:

> **Nothing is committed directly to `main`.** Every change arrives as a pull
> request, and the maintainer decides what gets merged.

You do not need permission to start. Fork it, build it, change something.

## The short version

```bash
# 1. Fork the repo on GitHub, then clone YOUR fork
git clone https://github.com/<your-username>/arena-fighter.git
cd arena-fighter

# 2. Branch. Never work on main.
git switch -c fix-sprint-lockout

# 3. Build and run
build.bat
ArenaFighter.exe

# 4. Test
tests\run_tests.bat

# 5. Commit and push to your fork
git add -A
git commit -m "Fix sprint lockout releasing one tick early"
git push -u origin fix-sprint-lockout
```

Then open a pull request against `main` on the upstream repo. GitHub offers a
"Compare & pull request" button right after you push.

## What you need

- Windows
- MSVC build tools (Visual Studio 2022 Build Tools, C++ workload)

raylib is vendored in `vendor/`, so there is no other setup. If `build.bat`
cannot find `vcvars64.bat`, fix the path on the one line near the top.

## Before you open the PR

- **It builds clean.** The project compiles at `/W4` with zero warnings. Keep
  it that way; a new warning is a review comment waiting to happen.
- **The tests pass.** Run `tests\run_tests.bat`. If you changed simulation
  behaviour, add a case — the tests are plain C and easy to extend.
- **One change per PR.** A focused twenty-line diff gets merged. A branch that
  fixes a bug, renames three things and reformats a file does not.
- **Say what and why.** "Reduced attack recovery from 0.36s to 0.28s; the
  finisher felt unresponsive when chained" tells the maintainer everything.
  "Tweaks" tells them nothing.

## House rules for the code

Match the surrounding code — it has a consistent voice, and that is worth more
than any individual preference. C17, four-space indent, no tabs. Comments
explain *why* a thing is done, not what the line already says.

Beyond style, a handful of rules keep the architecture intact. Breaking one of
these is the most likely reason a PR gets sent back:

1. **No randomness in the simulation.** No `rand()`, no `Math.random`-alikes
   anywhere reachable from `world_tick`. The simulation is deterministic on
   purpose. If something needs variety, derive it from existing state — the
   attack chain is an example of variety without randomness.
2. **No wall-clock time in the simulation.** Game logic uses `TICK_DT`, never
   `GetFrameTime()`. Anything that reads the real clock belongs in `main.c` or
   `render.c`.
3. **The renderer never mutates the simulation.** `render.c` takes `const`
   world state and draws it. The camera lives in `render.c` precisely because it
   must not be able to affect gameplay.
4. **New `Fighter` fields must be handled in `fighter_lerp`.** It builds a fresh
   struct field by field; forget one and the renderer reads uninitialised
   memory. This is the single easiest mistake to make in this codebase.
5. **Tuning constants go in a named `#define`, not inline.** Values the whole
   game shares (arena size, gravity, jump speed) live in `game.h`; values only
   one system cares about stay at the top of that `.c` file.
6. **Save-file changes need a version bump.** `settings.c` writes a versioned,
   checksummed record. If you change the on-disk layout, bump `SAVE_VERSION` —
   an old file is then rejected instead of misread.

## Good first contributions

- Make movement keys rebindable — the keybinds page is built, but only the
  cheat-menu key currently uses it (`settings.c`, `keybind_row`).
- Add a fourth attack to the chain. The swing table and pose switch in
  `fighter.c` / `render.c` are designed for it; it is a handful of lines.
- Tune the combat timings. `ATTACKS[]` in `fighter.c` holds startup/active/
  recovery for each swing, and those numbers are educated guesses.
- Port `build.bat` to a Makefile or CMake for Linux/macOS.

Larger things — the enemy AI, damage and hit reactions, the ability system —
are on the roadmap in the README. Please open an issue to talk it through
before writing a lot of code, so we do not both build the same thing twice.

## Reporting bugs

Open an issue with what you did, what happened, and what you expected. For
anything visual, a screenshot or a clip saves a lot of back-and-forth. Include
your GPU and whether V-Sync and the frame cap were on — a surprising number of
timing bugs only show up at particular frame rates.

## A note on merging

`main` is protected: it rejects direct pushes and force pushes, and requires a
pull request with maintainer approval. This applies to everyone. It is not a
statement about trust — it just means every change to the game has a review and
a paper trail.
