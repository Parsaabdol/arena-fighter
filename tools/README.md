# Turning a Dota 2 hero into a skin

This repository ships **no character art**, and it never will — see
[the note in `.gitignore`](../.gitignore). What it ships is the pipeline. If you
own Dota 2, these tools read a hero out of *your* installed copy and write a
playable skin into *your* `assets/` folder. Nothing is downloaded, nothing is
redistributed, and nothing of Valve's ever enters this repository.

---

## 1. Set up, once

Two things to install.

**Python 3.8 or newer.** From [python.org](https://www.python.org/downloads/).
Tick **"Add python.exe to PATH"** in the installer — the tools check for it and
will tell you if it is missing. No packages are needed; everything used is in
the standard library.

**Source2Viewer-CLI.** This is the extractor that can read Dota's archives.
Download the latest `cli` build from the
[ValveResourceFormat releases page](https://github.com/ValveResourceFormat/ValveResourceFormat/releases),
unzip it, and put `Source2Viewer-CLI.exe` in a folder called `cli` next to this
README:

```
arena-fighter/
  tools/
    cli/
      Source2Viewer-CLI.exe      <- here
```

It is a separate project under its own licence, which is why it is not bundled.
Anywhere on your `PATH` works too.

You do **not** need to tell anything where Dota is. Steam records its own
location, and the tools read it, then read Steam's library list to find which
drive Dota is actually on.

Finally, build the game once if you have not: **`build.bat`** in the repository
root. It produces `ArenaFighter.exe` and `SkinPreview.exe`, and the importer
needs the second one.

---

## 2. Your first skin

**Double-click `tools/import-heroes.bat`.**

A window opens with two lists on the left: **In your game** at the top, and
**Available** below it — every hero your Dota install has, sorted, and filtered
live by the Search box.

1. Type a few letters in **Search** — try `pud`.
2. Click **pudge** in the *Available* list.
3. Press **Export**.

The log on the right fills in as it works. It takes about a minute, and most of
that is the extractor. You will see it choose clips, list his gear slots, graft
each one on, and finally write the file:

```
clips (8 of 262 sequences):
   idle       pudge_idle_anim
   walk       pudge_walkN_anim
   run        pudge_run_haste_anim
   ...
  graft leftarm_model                  894 verts over 18 bones
  skip  weapon.vmdl_c                  (duplicate of a slot already taken)
wrote assets/pudge.glb  (8 meshes, 1 skin, 8 animations, self-contained)
```

When it finishes, a **preview window** opens on its own: Pudge on a turntable.

- **1**–**6** switch between idle, walk, sprint, crouch, attack and jump
- **drag** to turn him round, **wheel** to zoom, **space** to stop the spin
- the top line tells you how many of the game's 12 animation states found a clip

Close it when you have seen enough. Pudge has moved to the **In your game** list.

**Now start the game yourself** and choose him under **Customize → Mod Skins**.
The game re-reads `assets/` every time you open that page, so he is already
there even if the game was running the whole time.

That is the whole loop. Every other hero is the same three clicks.

---

## 3. The rest of the window

**Re-export** rebuilds a skin you already have — useful after changing anything
in the exporter.

**Delete** removes the `.glb`. If it happens to be the skin you have selected in
the game, the game quietly falls back to its built-in box fighter, so this is
always safe. You can export it again whenever.

**Preview** opens that turntable again for anything in *In your game* — including
files the tools did not make, like a hand-built arcana, which show up in the
list because they are simply files in `assets/`.

**Paths…** is only needed if something is somewhere unusual: it lets you point
straight at `pak01_dir.vpk` and at `Source2Viewer-CLI.exe`, and remembers them.

**Rescan** re-reads both your Dota install and `assets/`.

---

## 4. Without the window

`tools/export-hero.bat` asks for a hero name at a prompt; type `LIST` to see
them all. From a terminal:

```
python tools/export_hero.py pudge                 # -> assets/pudge.glb
python tools/export_hero.py --list                # every hero you have
python tools/export_hero.py pudge --dry-run       # decide nothing, just report
```

`--dry-run` is the one to reach for when something looks wrong: it prints the
clips and gear slots it *would* use without spending the minute.

Other flags:

- `--model PATH` — a body model that is not `models/heroes/<hero>/<hero>.vmdl_c`.
  Arcanas live under `models/items/`, so they need this, and slot scanning is
  switched off when you use it (that folder holds pedestals and effect models,
  not clothing) — list the pieces yourself with `--extra`.
- `--extra PATH` — one more item model to graft on. Repeatable.
- `--clips a,b,c` — override the clip choice entirely.
- `--out PATH` — somewhere other than `assets/<hero>.glb`.
- `--vpk PATH`, `--viewer PATH` — the same two paths the **Paths…** dialog sets.
  `DOTA_VPK` and `SOURCE2VIEWER` work as environment variables.

An arcana takes three of them:

```
python tools/export_hero.py ogre_magi_arcana \
    --model models/items/ogre_magi/ogre_arcana/ogre_magi_arcana.vmdl_c \
    --extra models/items/ogre_magi/ogre_arcana/ogre_magi_arcana_head.vmdl_c \
    --extra models/items/ogre_magi/ogre_arcana/ogre_magi_arcana_tail.vmdl_c
```

---

## 5. Why it is not just "run the extractor"

Four things go wrong if you export the obvious way, and each is why a step
exists. The reasoning is in [ARCHITECTURE.md §9](../ARCHITECTURE.md).

**A hero is not one model.** The body is one `vmdl` and every gear slot is
another, each carrying a small skin whose joints name a subset of the hero
skeleton, and no animations at all. Export the body alone and you get a hero
stripped of his gear — and, for Pudge, missing his left hand. `glb_merge.py`
grafts them onto one armature by bone name, moving a rigid prop's vertices when
its bind pose disagrees.

**Clips are chosen by activity, not by name.** A hero carries hundreds of
animations, most belonging to cosmetic sets sold for him, and the good ones are
not reliably the obvious names. Picking "the sequence called `flail`" gets you
`ACT_DOTA_FLAIL_STATUE` — a petrified topple — on the heroes that call the
airborne one `flail_anim`. So selection reads the activity table and uses names
only to break ties, preferring clips that *start* with the hero's own name,
since a cosmetic's variant keeps the name and prefixes itself onto the front.

**Not every slot fits the hero.** Some props carry their own little skeleton
rather than being skinned to his — Necrophos's Reaper's Scythe is rigged to
`root/bind/pivot/anim`, and the game places that sort of thing by attachment
point, which nothing here can reproduce. Those slots are reported and skipped;
losing one piece beats losing the hero. Capitalisation is not such a case:
Valve's own item models disagree with their bodies about it (`spine_3` against
`Spine_3`), so bone lookup folds case.

**Slot lists contain things you must not load.** Duplicates: Pudge ships his
hook as both `weapon` and `righthook`, same material and same bounding box, and
loading both z-fights. Non-clothing: `witchdoctor_ward` is a summoned entity and
`witchdoctor_old` a legacy model. Style variants: an arcana head ships its
geometry three times as `_style1` and `_dummy`. All three are dropped —
duplicates by comparing mesh signatures, the rest by name.

### About the preview

`SkinPreview.exe` is built by `build.bat` from the game's own `model.c`,
`anim_gltf.c` and `fighter.c` plus a small `main`. It is not a picture of the
game and not a viewer of its own: how a skin ends up looking is the product of
the loader's bind-pose rewrite, the up-axis vote, the fit that measures the idle
pose and ignores what he carries, and the clip the alias tables resolve for each
state — and the preview runs all four, because it *is* that code. A separate
implementation would drift until the preview started lying.

It is deliberately not the game, either: no arena, no simulation, no settings
file, so looking at a skin can never change what you play as.

---

## 6. If something goes wrong

| what you see | what to do |
| --- | --- |
| "Python was not found" | install it, ticking *Add python.exe to PATH* |
| "could not find Source2Viewer-CLI" | put it in `tools/cli/`, or set it under **Paths…** |
| "could not find Dota 2" | set `pak01_dir.vpk` under **Paths…** |
| "SkinPreview.exe is not in the repo root" | run `build.bat` |
| hero runs backwards | `MODEL_YAW` in `src/model.c` — facing is per-pack |
| stands in his idle pose while moving | the preview and the Mod Skins page both report how many states matched; teach `CLIP_ALIASES` in `src/model.c` the pack's names |
| swings land off the beat | `CLIP_STRIKE_POINT` in `src/model.c` |
| floats, sinks, or is the wrong size | `pose_bounding_box` in `src/model.c` |
| a piece of gear is missing | the log says which slot and why; a prop on its own rig cannot be grafted |
| slow to load in game | too many clips — see ARCHITECTURE.md §12 |

---

## 7. The files

- `import-heroes.bat` / `hero_importer.py` — the window. A launcher: it browses,
  exports, deletes, and shells out to `SkinPreview.exe`. Remembers your paths in
  `importer-config.json`, which is gitignored because it is per-machine.
- `export-hero.bat` / `export_hero.py` — the driver: find Dota, choose clips,
  collect slots, export, merge. Everything the window does, minus the window.
- `glb_merge.py` — the merger. Useful on its own for any body-plus-items glTF
  set, Dota or not.
