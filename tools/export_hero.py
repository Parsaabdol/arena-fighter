"""
Build a playable arena-fighter skin from a Dota 2 hero you already own.

    python tools/export_hero.py ogre_magi

Nothing of Valve's is redistributed by this repository: the models are read out
of YOUR installed copy of the game and written to `assets/`, which is
gitignored. See tools/README.md for what you need installed first.

What it does, in the order it matters:

  1. reads the hero's sequence table and picks clips BY ACTIVITY, not by name
  2. collects the body plus every gear slot, discarding duplicates and props
  3. exports each through Source2Viewer-CLI
  4. grafts them onto one armature with glb_merge

Step 1 is the one worth understanding. A Dota hero carries hundreds of clips
and the useful ones are not reliably named -- picking "the sequence called
flail" gets you ACT_DOTA_FLAIL_STATUE, a petrified topple, on the heroes that
name the airborne one `flail_anim`. The activity table is the actual meaning,
so selection reads that and uses names only to break ties.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import glb_merge                                          # noqa: E402


# ------------------------------------------------------------ what we want ---
#
# Each row is one clip the game can actually reach, paired with the Dota
# activity that means it. The NAME the clip ends up with still matters -- the
# game matches clips by name against CLIP_ALIASES in src/model.c -- so the
# comment on each row is the alias it is expected to satisfy.

ROLES = [
    # The third column is name hints, tried in order; None means "no hint".
    # A row with no None never settles, so it contributes nothing when the hero
    # has no such clip.
    ("idle",    ["ACT_DOTA_IDLE"], [None]),         # ANIM_IDLE, and every fallback
    # Dota gives walking and running one activity, and most heroes only run.
    # Where a hero does have both -- Pudge is slow enough to walk -- taking each
    # separately is what lets ANIM_WALK and ANIM_SPRINT differ.
    ("walk",    ["ACT_DOTA_RUN"], ["walk"]),        # ANIM_WALK, when it exists
    ("run",     ["ACT_DOTA_RUN"], ["run", None]),   # ANIM_WALK's fallback
    # Dota has no sprint either, but most heroes carry a faster run authored
    # for the Haste rune, and that is the pose a sprint wants. Optional: no
    # None fallback, because without one ANIM_SPRINT is happy with the run.
    ("sprint",  ["ACT_DOTA_RUN"], ["haste"]),       # ANIM_SPRINT
    ("attack",  ["ACT_DOTA_ATTACK"], [None]),       # ANIM_ATTACK + the swing chain
    # Activity finds the right clips; the hints then prefer one the GAME can
    # find, because it matches by name. Pudge's cast is `meathook_start` and
    # Witch Doctor's is `paralyzing_cask` -- both correct by activity, neither
    # reachable by any ANIM_SPECIAL alias. The hints below are those aliases.
    ("cast",    [r"ACT_DOTA_(CAST|CHANNEL|OVERRIDE)_ABILITY_\d+",   # ANIM_SPECIAL
                 r"ACT_DOTA_GENERIC_CHANNEL"],
                ["cast", "ability", "spell", "channel", None]),
    ("flail",   ["ACT_DOTA_FLAIL"], [None]),        # the airborne states
    ("death",   ["ACT_DOTA_DIE"], ["death", "die", None]),   # ANIM_HURT
]

# The attack chain wants three clips, and only names distinguish them: every
# swing is ACT_DOTA_ATTACK. src/model.c's SWING_ALIASES looks for these.
SWING_HINTS = ["attack1", "attack2", "attack3"]

# Substrings that mark a clip as a worse choice than an otherwise equal one.
# This mirrors AVOID in src/model.c and exists for the same reason: a hero
# export carries a decade of cosmetic animations named after him.
DEMOTE = [
    "portrait", "loadout", "victory", "defeat", "taunt", "spawn", "versus",
    "injured", "effigy", "rare", "haste", "chase", "immortal", "prestige",
    "persona", "bonkers", "ti6", "ti7", "ti8", "ti9", "ti10", "ti11",
    "_old", "generic", "alt", "_end",
]

# Gear slots that are not worn. A hero's model folder holds more than his
# clothes: summoned entities, legacy models, and "refit" variants meant for a
# different cosmetic set.
SKIP_SLOTS = ["_refit", "_old", "_ward", "_statue", "_death", "_bg"]


# ------------------------------------------------------------------ finding ---

def find_dota(explicit=None):
    """Locate pak01_dir.vpk, preferring an explicit path, then $DOTA_VPK, then
    every library Steam knows about."""
    for cand in (explicit, os.environ.get("DOTA_VPK")):
        if cand:
            if not os.path.exists(cand):
                raise SystemExit("no VPK at %s" % cand)
            return cand

    roots = []
    # Ask Windows where Steam actually is before guessing. Steam records it,
    # and guessing only covers the default install -- someone with Steam on
    # D:\ would otherwise be told to pass --vpk for no reason.
    try:
        import winreg
        for hive, key, name in (
                (winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam", "SteamPath"),
                (winreg.HKEY_LOCAL_MACHINE,
                 r"SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath")):
            try:
                with winreg.OpenKey(hive, key) as k:
                    roots.append(os.path.normpath(winreg.QueryValueEx(k, name)[0]))
            except OSError:
                pass
    except ImportError:
        pass                                  # not Windows

    roots += [os.path.expandvars(r"%ProgramFiles(x86)%\Steam"),
              os.path.expandvars(r"%ProgramFiles%\Steam"),
              os.path.expanduser("~/.steam/steam"),
              os.path.expanduser("~/.local/share/Steam"),
              os.path.expanduser("~/Library/Application Support/Steam")]
    libraries = []
    for root in roots:
        vdf = os.path.join(root, "steamapps", "libraryfolders.vdf")
        if not os.path.exists(vdf):
            continue
        libraries.append(root)
        with open(vdf, encoding="utf-8", errors="replace") as f:
            libraries += re.findall(r'"path"\s+"([^"]+)"', f.read())

    for lib in libraries:
        p = os.path.join(lib.replace("\\\\", "\\"), "steamapps", "common",
                         "dota 2 beta", "game", "dota", "pak01_dir.vpk")
        if os.path.exists(p):
            return p
    raise SystemExit(
        "could not find Dota 2. Pass --vpk with the path to pak01_dir.vpk, "
        "or set DOTA_VPK.")


def find_viewer(explicit=None):
    """Locate Source2Viewer-CLI."""
    names = ["Source2Viewer-CLI.exe", "Source2Viewer-CLI"]
    for cand in (explicit, os.environ.get("SOURCE2VIEWER")):
        if cand:
            if not os.path.exists(cand):
                raise SystemExit("no Source2Viewer-CLI at %s" % cand)
            return cand
    here = os.path.dirname(os.path.abspath(__file__))
    for d in (here, os.path.join(here, "cli"), os.path.join(here, "..", ".."),
              os.path.join(here, "..", "..", "tools", "cli")):
        for n in names:
            p = os.path.join(d, n)
            if os.path.exists(p):
                return os.path.abspath(p)
    for d in os.environ.get("PATH", "").split(os.pathsep):
        for n in names:
            p = os.path.join(d, n)
            if os.path.exists(p):
                return p
    raise SystemExit(
        "could not find Source2Viewer-CLI. Download it from "
        "https://github.com/ValveResourceFormat/ValveResourceFormat/releases "
        "and pass --viewer, set SOURCE2VIEWER, or drop it in tools/cli/.")


def run(cli, args):
    out = subprocess.run([cli] + args, capture_output=True, text=True)
    return (out.stdout or "") + (out.stderr or "")


# ------------------------------------------------------------------ reading ---

def list_models(cli, vpk, prefix):
    text = run(cli, ["-i", vpk, "-l", "-f", prefix, "-e", "vmdl_c"])
    found = []
    for line in text.splitlines():
        line = line.strip()
        if line.endswith(".vmdl_c") or ".vmdl_c " in line:
            found.append(line.split()[0])
    return sorted(set(found))


def read_sequences(cli, vpk, model):
    """Return [(name, [activities]), ...] for one vmdl.

    Source2Viewer prints the ASEQ block as KV3 text. A sequence starts at its
    `m_sName` and owns every `m_name` until the next one; the ones beginning
    ACT_ are activities, the rest are modifiers like "fast" that we ignore.
    """
    text = run(cli, ["-i", vpk, "-f", model, "-b", "ASEQ"])
    seqs, cur = [], None
    for line in text.splitlines():
        line = line.strip()
        m = re.match(r'm_sName\s*=\s*"([^"]*)"', line)
        if m:
            name = m.group(1)
            cur = None
            if name and not name.startswith("!"):
                cur = (name, [])
                seqs.append(cur)
            continue
        m = re.match(r'm_name\s*=\s*"(ACT_[A-Z0-9_]+)"', line)
        if m and cur is not None:
            cur[1].append(m.group(1))
    return seqs


def score(name, hero):
    """Lower is better.

    The hero's OWN name does the heavy lifting, exactly as clip_score does in
    src/model.c. A hero export carries every animation ever authored for a
    cosmetic set worn by him, under short unrelated names -- Pudge has `hh_idle`
    and `trapper_walk` -- and those beat `pudge_idle_anim` on length alone.
    Length only separates a plain move from a qualified one after that.
    """
    s = len(name)
    low = name.lower()
    if hero and low.startswith(hero):
        # Valve names a hero's own clips `<hero>_<move>`; a cosmetic set's
        # variant of the same move keeps that and prefixes itself on the front,
        # so `hh_pudge_walkN` is shorter than `pudge_walkN_anim` and would
        # otherwise win. Where the name STARTS decides it.
        s -= 2000
    elif hero and hero in low:
        s -= 1000
    for d in DEMOTE:
        if d in low:
            s += 100
    return s


def pick(seqs, activities, hero, hints=(None,)):
    """Best sequence carrying any of `activities`. Hints are tried in order and
    the first that matches anything wins, so a role can ask for a named variant
    and still settle for the plain one."""
    # Activities are full-match patterns, which is what keeps the near-misses
    # apart: ACT_DOTA_FLAIL must not catch ACT_DOTA_FLAIL_STATUE, nor
    # ACT_DOTA_IDLE catch ACT_DOTA_IDLE_RARE.
    def carries(acts):
        return any(re.fullmatch(pat, a) for pat in activities for a in acts)

    for hint in hints:
        best, best_s = None, None
        for name, acts in seqs:
            if not carries(acts):
                continue
            if hint and hint not in name.lower():
                continue
            s = score(name, hero)
            if best is None or s < best_s:
                best, best_s = name, s
        if best:
            return best
    return None


def choose_clips(seqs, hero):
    chosen, why = [], []
    for role, acts, hints in ROLES:
        n = pick(seqs, acts, hero, hints=hints)
        if n and n not in chosen:
            chosen.append(n)
            why.append((role, n))
        elif n:
            why.append((role, n + "   (already listed)"))
        else:
            why.append((role, None))
    for hint in SWING_HINTS:
        n = pick(seqs, ["ACT_DOTA_ATTACK"], hero, hints=[hint])
        label = "swing " + hint[-1]
        if not n:
            why.append((label, None))
        elif n in chosen:
            # Already exported for another role -- one clip, two jobs.
            why.append((label, n + "   (already listed)"))
        else:
            chosen.append(n)
            why.append((label, n))
    return chosen, why


# ---------------------------------------------------------------- exporting ---

def export(cli, vpk, model, outdir, clips):
    args = ["-i", vpk, "-f", model, "-o", outdir, "-d",
            "--gltf_export_format", "glb", "--gltf_export_materials",
            "--gltf_textures_adapt", "--gltf_export_animations"]
    if clips:
        args += ["--gltf_animation_list", ",".join(clips)]
    run(cli, args)
    stem = os.path.splitext(os.path.basename(model))[0]
    for root, _dirs, files in os.walk(outdir):
        for f in files:
            if f == stem + ".glb":
                return os.path.join(root, f)
    return None


def mesh_signature(path):
    """(material, rounded bbox) per mesh -- enough to spot two slots that are
    the same prop. Pudge ships his hook as both `weapon` and `righthook`, and
    loading both z-fights."""
    g, _ = glb_merge.read_glb(path)
    sigs = []
    for mesh in g.get("meshes", []):
        for prim in mesh["primitives"]:
            acc = g["accessors"][prim["attributes"]["POSITION"]]
            mat = g["materials"][prim["material"]].get("name", "") \
                if "material" in prim else ""
            box = tuple(round(v, 1) for v in
                        (acc.get("min", []) + acc.get("max", [])))
            sigs.append((mat, box))
    return tuple(sigs)


# -------------------------------------------------------------------- main ---

def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("hero", nargs="?",
                    help="folder name under models/heroes, e.g. pudge")
    ap.add_argument("--list", action="store_true",
                    help="print every hero this Dota install has, and stop")
    ap.add_argument("--model", help="body vmdl path, if not models/heroes/<hero>/<hero>.vmdl_c")
    ap.add_argument("--extra", action="append", default=[],
                    help="another item vmdl to graft on (repeatable)")
    ap.add_argument("--clips", help="comma-separated clip names, overriding the activity pick")
    ap.add_argument("--out", help="output .glb (default assets/<hero>.glb)")
    ap.add_argument("--vpk", help="path to pak01_dir.vpk")
    ap.add_argument("--viewer", help="path to Source2Viewer-CLI")
    ap.add_argument("--dry-run", action="store_true",
                    help="report the clips and slots that would be used, then stop")
    args = ap.parse_args()

    vpk = find_dota(args.vpk)
    cli = find_viewer(args.viewer)

    if args.list:
        # A hero's folder is named for him, and the body model inside repeats
        # that name. Anything else in models/heroes is a prop or a leftover.
        names = set()
        for m in list_models(cli, vpk, "models/heroes/"):
            parts = m.split("/")
            if len(parts) >= 4 and parts[3] == parts[2] + ".vmdl_c":
                names.add(parts[2])
        print("%d heroes in this Dota install:\n" % len(names))
        col = sorted(names)
        for i in range(0, len(col), 3):
            print("   " + "".join("%-26s" % n for n in col[i:i + 3]))
        print("\nexport one with:  python tools/export_hero.py <name>")
        return

    if not args.hero:
        raise SystemExit("which hero? try --list to see the names")

    body = args.model or "models/heroes/%s/%s.vmdl_c" % (args.hero, args.hero)
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = args.out or os.path.join(repo, "assets", args.hero + ".glb")

    print("dota   %s" % vpk)
    print("viewer %s" % cli)
    print("body   %s" % body)

    seqs = read_sequences(cli, vpk, body)
    if not seqs:
        raise SystemExit("no sequences in %s -- is the path right?" % body)
    if args.clips:
        clips, why = args.clips.split(","), []
    else:
        clips, why = choose_clips(seqs, args.hero.lower())
    print("\nclips (%d of %d sequences):" % (len(clips), len(seqs)))
    for role, name in why:
        print("   %-10s %s" % (role, name if name else "(none in this hero)"))

    # Gear slots live beside the body model, and everything in that folder that
    # is not the body is a candidate. That only holds under models/heroes/,
    # where the folder IS the hero's wardrobe. A cosmetic set's folder holds
    # pedestals, effect models and a debut camera rig alongside the wearables,
    # so when --model points elsewhere the caller lists the pieces with --extra.
    if args.model:
        slots = []
        print("\nslots: none scanned (--model given; use --extra)")
    else:
        folder = body.rsplit("/", 1)[0] + "/"
        slots = [m for m in list_models(cli, vpk, folder)
                 if m != body and not any(s in m.lower() for s in SKIP_SLOTS)]
        print("\nslots (%d):" % len(slots))
        for s in slots:
            print("   %s" % s.rsplit("/", 1)[-1])
    for e in args.extra:
        print("   %s  (--extra)" % e.rsplit("/", 1)[-1])

    if args.dry_run:
        print("\ndry run -- nothing exported")
        return

    with tempfile.TemporaryDirectory(prefix="arena-export-") as tmp:
        print("\nexporting...")
        body_glb = export(cli, vpk, body, tmp, clips)
        if not body_glb:
            raise SystemExit("Source2Viewer produced no glb for %s" % body)

        items, seen = [], {mesh_signature(body_glb)}
        for m in slots + args.extra:
            p = export(cli, vpk, m, tmp, [])
            if not p:
                print("   skip %-28s (no glb produced)" % m.rsplit("/", 1)[-1])
                continue
            sig = mesh_signature(p)
            if sig in seen:
                print("   skip %-28s (duplicate of a slot already taken)"
                      % m.rsplit("/", 1)[-1])
                continue
            seen.add(sig)
            items.append(p)

        print("\nmerging...")
        os.makedirs(os.path.dirname(out), exist_ok=True)
        # Style variants ship as extra copies of the same geometry; keeping
        # more than one z-fights.
        glb_merge.merge_files(body_glb, items, out,
                              drop_mesh=["_style1", "_dummy", "_lod1"])

    print("\ndone -- pick %s under Customize / Mod Skins"
          % os.path.basename(out))


if __name__ == "__main__":
    main()
