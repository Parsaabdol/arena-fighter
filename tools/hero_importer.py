"""
A window for turning Dota heroes you own into Arena Fighter skins.

    python tools/hero_importer.py        (or double-click import-heroes.bat)

Two lists: what is in your game, and what you could add. Export writes a .glb
into assets/; Preview opens SkinPreview.exe, which links the game's own loader
so what you see is what the game will draw. It never starts the game and never
touches your saved settings -- you launch the game yourself, when you want to.
"""
import json
import os
import queue
import subprocess
import sys
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
ASSETS = os.path.join(REPO, "assets")
PREVIEW = os.path.join(REPO, "SkinPreview.exe")
EXPORTER = os.path.join(HERE, "export_hero.py")
CONFIG = os.path.join(HERE, "importer-config.json")

sys.path.insert(0, HERE)


def load_config():
    try:
        with open(CONFIG, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def save_config(cfg):
    try:
        with open(CONFIG, "w", encoding="utf-8") as f:
            json.dump(cfg, f, indent=2)
    except OSError:
        pass


class Importer(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Arena Fighter — Hero Import")
        self.geometry("940x620")
        self.minsize(820, 520)

        self.cfg = load_config()
        self._apply_config()

        self.heroes = []
        self.events = queue.Queue()
        self.busy = False
        self.current = None          # (hero, installed?)

        self._build()
        self.after(80, self._pump)
        self.reload()

    # ------------------------------------------------------------- config ---

    def _apply_config(self):
        """The exporter reads these from the environment, so setting them here
        covers both the in-process hero listing and the export subprocess."""
        if self.cfg.get("vpk"):
            os.environ["DOTA_VPK"] = self.cfg["vpk"]
        if self.cfg.get("viewer"):
            os.environ["SOURCE2VIEWER"] = self.cfg["viewer"]

    # ----------------------------------------------------------------- ui ---

    def _build(self):
        root = ttk.Frame(self, padding=10)
        root.pack(fill="both", expand=True)
        root.columnconfigure(0, minsize=260)
        root.columnconfigure(1, weight=1)
        root.rowconfigure(1, weight=1)

        top = ttk.Frame(root)
        top.grid(row=0, column=0, columnspan=2, sticky="ew", pady=(0, 8))
        ttk.Label(top, text="Search").pack(side="left")
        self.search = tk.StringVar()
        self.search.trace_add("write", lambda *_: self._refill())
        ttk.Entry(top, textvariable=self.search, width=28).pack(
            side="left", padx=(6, 12))
        ttk.Button(top, text="Paths…", command=self.on_paths).pack(side="right")
        ttk.Button(top, text="Rescan", command=self.reload).pack(
            side="right", padx=(0, 6))

        left = ttk.Frame(root)
        left.grid(row=1, column=0, sticky="nsew")
        left.rowconfigure(1, weight=0)
        left.rowconfigure(3, weight=1)
        left.columnconfigure(0, weight=1)

        self.h_installed = ttk.Label(left, text="In your game",
                                     font=("Segoe UI", 9, "bold"))
        self.h_installed.grid(row=0, column=0, sticky="w")
        self.installed = tk.Listbox(left, activestyle="none", height=7,
                                    exportselection=False)
        self.installed.grid(row=1, column=0, sticky="ew", pady=(2, 10))
        self.installed.bind("<<ListboxSelect>>",
                            lambda _e: self._pick(self.installed, True))
        self.installed.bind("<Double-Button-1>", lambda _e: self.on_preview())

        self.h_available = ttk.Label(left, text="Available",
                                     font=("Segoe UI", 9, "bold"))
        self.h_available.grid(row=2, column=0, sticky="w")
        self.available = tk.Listbox(left, activestyle="none",
                                    exportselection=False)
        self.available.grid(row=3, column=0, sticky="nsew", pady=(2, 0))
        self.available.bind("<<ListboxSelect>>",
                            lambda _e: self._pick(self.available, False))
        self.available.bind("<Double-Button-1>", lambda _e: self.on_export())
        bar = ttk.Scrollbar(left, orient="vertical", command=self.available.yview)
        bar.grid(row=3, column=0, sticky="nse", pady=(2, 0))
        self.available.config(yscrollcommand=bar.set)

        right = ttk.Frame(root, padding=(14, 0, 0, 0))
        right.grid(row=1, column=1, sticky="nsew")
        right.rowconfigure(3, weight=1)
        right.columnconfigure(0, weight=1)

        self.title_lbl = ttk.Label(right, text="Loading…",
                                   font=("Segoe UI", 15, "bold"))
        self.title_lbl.grid(row=0, column=0, sticky="w")
        self.status_lbl = ttk.Label(right, text="", foreground="#666")
        self.status_lbl.grid(row=1, column=0, sticky="w", pady=(2, 10))

        btns = ttk.Frame(right)
        btns.grid(row=2, column=0, sticky="ew")
        self.b_export = ttk.Button(btns, text="Export", command=self.on_export,
                                   state="disabled")
        self.b_preview = ttk.Button(btns, text="Preview",
                                    command=self.on_preview, state="disabled")
        self.b_delete = ttk.Button(btns, text="Delete", command=self.on_delete,
                                   state="disabled")
        for b in (self.b_export, self.b_preview, self.b_delete):
            b.pack(side="left", padx=(0, 6))
        ttk.Button(btns, text="Open assets folder",
                   command=self.on_folder).pack(side="left")

        self.log = tk.Text(right, height=12, wrap="none", state="disabled",
                           font=("Consolas", 9), background="#1e1e1e",
                           foreground="#d4d4d4", relief="flat")
        self.log.grid(row=3, column=0, sticky="nsew", pady=(10, 0))

        self.auto = tk.BooleanVar(value=self.cfg.get("auto_preview", True))
        ttk.Checkbutton(right, text="Open the preview when an export finishes",
                        variable=self.auto,
                        command=self._save_auto).grid(row=4, column=0,
                                                      sticky="w", pady=(8, 0))

    def _save_auto(self):
        self.cfg["auto_preview"] = self.auto.get()
        save_config(self.cfg)

    def say(self, line):
        self.log.config(state="normal")
        self.log.insert("end", line.rstrip() + "\n")
        self.log.see("end")
        self.log.config(state="disabled")

    # -------------------------------------------------------------- state ---

    def path_of(self, hero):
        return os.path.join(ASSETS, hero + ".glb")

    def is_installed(self, hero):
        return os.path.exists(self.path_of(hero))

    def reload(self):
        self.title_lbl.config(text="Loading…")
        self.status_lbl.config(text="reading your Dota install")
        threading.Thread(target=self._load, daemon=True).start()

    def _load(self):
        try:
            import export_hero
            import importlib
            importlib.reload(export_hero)         # pick up edited paths
            vpk = export_hero.find_dota(self.cfg.get("vpk"))
            cli = export_hero.find_viewer(self.cfg.get("viewer"))
            names = set()
            for m in export_hero.list_models(cli, vpk, "models/heroes/"):
                parts = m.split("/")
                if len(parts) >= 4 and parts[3] == parts[2] + ".vmdl_c":
                    names.add(parts[2])
            self.events.put(("heroes", (sorted(names), vpk)))
        except SystemExit as e:
            self.events.put(("fatal", str(e)))
        except Exception as e:                       # noqa: BLE001
            self.events.put(("fatal", "%s: %s" % (type(e).__name__, e)))

    def _refill(self):
        """Both lists, filtered by the search box and sorted. Installed skins
        are listed even when Dota is unreachable -- they are files on disk and
        do not depend on the game being found."""
        term = self.search.get().strip().lower()
        keep_i = self.current[0] if self.current else None

        installed = sorted(h for h in self._known() if self.is_installed(h))
        available = sorted(h for h in self.heroes if not self.is_installed(h))

        self.installed.delete(0, "end")
        for h in installed:
            if not term or term in h:
                self.installed.insert("end", h)
        self.available.delete(0, "end")
        for h in available:
            if not term or term in h:
                self.available.insert("end", h)

        self.h_installed.config(text="In your game  (%d)" % len(installed))
        self.h_available.config(text="Available  (%d)" % len(available))
        if keep_i:
            self._reselect(keep_i)

    def _known(self):
        """Hero names plus anything already sitting in assets/, so a skin whose
        hero is not in the list -- an arcana, a hand-made file -- is still
        manageable here."""
        names = set(self.heroes)
        if os.path.isdir(ASSETS):
            for f in os.listdir(ASSETS):
                if f.lower().endswith((".glb", ".gltf")):
                    names.add(os.path.splitext(f)[0])
        return names

    def _reselect(self, hero):
        for box, inst in ((self.installed, True), (self.available, False)):
            for i in range(box.size()):
                if box.get(i) == hero:
                    box.selection_clear(0, "end")
                    box.selection_set(i)
                    box.see(i)
                    self.current = (hero, inst)
                    self._describe()
                    return
        self.current = None
        self._describe()

    def _pick(self, box, installed):
        sel = box.curselection()
        if not sel:
            return
        other = self.available if box is self.installed else self.installed
        other.selection_clear(0, "end")
        self.current = (box.get(sel[0]), installed)
        self._describe()

    def _describe(self):
        if not self.current:
            self.title_lbl.config(text="Pick a hero")
            self.status_lbl.config(text="")
        else:
            hero, _ = self.current
            self.title_lbl.config(text=hero)
            if self.is_installed(hero):
                mb = os.path.getsize(self.path_of(hero)) / 1048576.0
                self.status_lbl.config(
                    text="in your game — assets/%s.glb, %.1f MB" % (hero, mb))
            else:
                self.status_lbl.config(text="not exported yet")
        self._sync()

    def _sync(self):
        hero = self.current[0] if self.current else None
        have = bool(hero) and self.is_installed(hero)
        free = not self.busy
        self.b_export.config(
            text="Re-export" if have else "Export",
            state="normal" if (free and hero and hero in self.heroes) else "disabled")
        self.b_preview.config(state="normal" if (free and have) else "disabled")
        self.b_delete.config(state="normal" if (free and have) else "disabled")

    # ------------------------------------------------------------ actions ---

    def on_export(self):
        if self.busy or not self.current:
            return
        hero = self.current[0]
        if hero not in self.heroes:
            return
        self.busy = True
        self._sync()
        self.say("")
        self.say("=== exporting %s ===" % hero)
        threading.Thread(target=self._run, args=(hero,), daemon=True).start()

    def _run(self, hero):
        try:
            p = subprocess.Popen(
                [sys.executable, EXPORTER, hero],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, encoding="utf-8", errors="replace", cwd=REPO,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            for line in p.stdout:
                self.events.put(("log", line))
            p.wait()
            self.events.put(("done", (hero, p.returncode)))
        except Exception as e:                       # noqa: BLE001
            self.events.put(("log", "could not start the exporter: %s" % e))
            self.events.put(("done", (hero, 1)))

    def on_preview(self):
        if not self.current or not self.is_installed(self.current[0]):
            return
        hero = self.current[0]
        if not os.path.exists(PREVIEW):
            messagebox.showerror(
                "Not built",
                "SkinPreview.exe is not in the repo root.\nRun build.bat.")
            return
        self.say("preview: %s" % hero)
        subprocess.Popen([PREVIEW, self.path_of(hero)], cwd=REPO)

    def on_delete(self):
        if not self.current or not self.is_installed(self.current[0]):
            return
        hero = self.current[0]
        if not messagebox.askyesno(
                "Delete skin",
                "Remove assets/%s.glb from your game?\n\nIf it is the skin you "
                "have selected, the game falls back to the built-in fighter. "
                "You can export it again at any time." % hero):
            return
        try:
            os.remove(self.path_of(hero))
        except OSError as e:
            messagebox.showerror(
                "Could not delete",
                "%s\n\nClose the game or the preview if either has this skin "
                "open, then try again." % e)
            return
        self.say("deleted assets/%s.glb" % hero)
        self._refill()
        self._reselect(hero)

    def on_folder(self):
        os.makedirs(ASSETS, exist_ok=True)
        if sys.platform == "win32":
            os.startfile(ASSETS)                     # noqa: S606
        elif sys.platform == "darwin":
            subprocess.Popen(["open", ASSETS])
        else:
            subprocess.Popen(["xdg-open", ASSETS])

    def on_paths(self):
        PathsDialog(self)

    # --------------------------------------------------------------- pump ---

    def _pump(self):
        try:
            while True:
                kind, payload = self.events.get_nowait()
                if kind == "log":
                    self.say(payload)
                elif kind == "heroes":
                    names, vpk = payload
                    self.heroes = names
                    self._refill()
                    self._describe()
                    self.say("Dota: %s" % vpk)
                    self.say("%d heroes available" % len(names))
                elif kind == "fatal":
                    self.heroes = []
                    self._refill()
                    self.title_lbl.config(text="Dota not found")
                    self.status_lbl.config(
                        text="set the paths, then press Rescan")
                    self.say(payload)
                elif kind == "done":
                    hero, code = payload
                    self.busy = False
                    self._refill()
                    self._reselect(hero)
                    if code == 0 and self.is_installed(hero):
                        self.say("=== %s added to your game ===" % hero)
                        if self.auto.get():
                            self.on_preview()
                    else:
                        self.say("=== %s failed (exit %d) ===" % (hero, code))
        except queue.Empty:
            pass
        self.after(80, self._pump)


class PathsDialog(tk.Toplevel):
    """Where Dota and the extractor are. Normally both are found automatically
    -- Steam records its own location in the registry -- so this is for the
    installs that are somewhere unusual."""

    def __init__(self, parent):
        super().__init__(parent)
        self.parent = parent
        self.title("Paths")
        self.resizable(False, False)
        self.transient(parent)
        self.grab_set()

        frm = ttk.Frame(self, padding=14)
        frm.pack(fill="both", expand=True)

        self.vpk = tk.StringVar(value=parent.cfg.get("vpk", ""))
        self.viewer = tk.StringVar(value=parent.cfg.get("viewer", ""))

        self._row(frm, 0, "Dota 2 pak01_dir.vpk", self.vpk,
                  [("Dota archive", "pak01_dir.vpk")],
                  "leave blank to find it through Steam")
        self._row(frm, 2, "Source2Viewer-CLI", self.viewer,
                  [("Executable", "*.exe")],
                  "leave blank to look on PATH and in tools/cli/")

        bar = ttk.Frame(frm)
        bar.grid(row=4, column=0, columnspan=3, sticky="e", pady=(14, 0))
        ttk.Button(bar, text="Cancel", command=self.destroy).pack(side="right")
        ttk.Button(bar, text="Save and rescan",
                   command=self.save).pack(side="right", padx=(0, 6))

    def _row(self, frm, r, label, var, types, hint):
        ttk.Label(frm, text=label).grid(row=r, column=0, sticky="w")
        ttk.Entry(frm, textvariable=var, width=58).grid(
            row=r, column=1, sticky="ew", padx=(10, 6))
        ttk.Button(frm, text="Browse…",
                   command=lambda: self._browse(var, types)).grid(row=r, column=2)
        ttk.Label(frm, text=hint, foreground="#888").grid(
            row=r + 1, column=1, sticky="w", padx=(10, 0), pady=(0, 10))

    def _browse(self, var, types):
        p = filedialog.askopenfilename(parent=self, filetypes=types)
        if p:
            var.set(os.path.normpath(p))

    def save(self):
        for key, var, env in (("vpk", self.vpk, "DOTA_VPK"),
                              ("viewer", self.viewer, "SOURCE2VIEWER")):
            value = var.get().strip()
            if value:
                self.parent.cfg[key] = value
            else:
                self.parent.cfg.pop(key, None)
                os.environ.pop(env, None)
        save_config(self.parent.cfg)
        self.parent._apply_config()
        self.destroy()
        self.parent.reload()


if __name__ == "__main__":
    Importer().mainloop()
