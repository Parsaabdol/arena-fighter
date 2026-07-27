#include "settings.h"
#include "ui.h"

#include "raylib.h"

#include <stddef.h>   /* offsetof */
#include <stdint.h>
#include <stdio.h>

/* ---------------------------- option tables ------------------------------ */

/* Discrete frame caps. 0 means "unlimited" and is deliberately last so the
 * slider running all the way right reads as "no limit". */
static const int FPS_OPTIONS[] = {
    30, 60, 75, 90, 120, 144, 165, 180, 200, 240, 300, 360, 480, 600, 0
};
#define FPS_COUNT ((int)(sizeof(FPS_OPTIONS) / sizeof(FPS_OPTIONS[0])))
#define FPS_DEFAULT_INDEX 4      /* 120 */

static const int RESOLUTIONS[][2] = {
    { 1280,  720 },
    { 1366,  768 },
    { 1600,  900 },
    { 1920, 1080 },
    { 2560, 1440 },
    { 3440, 1440 },
    { 3840, 2160 },
};
#define RES_COUNT ((int)(sizeof(RESOLUTIONS) / sizeof(RESOLUTIONS[0])))
#define RES_DEFAULT_INDEX 0      /* 1280x720 */

static const char *const RES_LABELS[] = {
    "1280 x 720",
    "1366 x 768",
    "1600 x 900",
    "1920 x 1080  (1080p)",
    "2560 x 1440  (1440p)",
    "3440 x 1440  (ultrawide)",
    "3840 x 2160  (4K)",
};

/* Radians of camera rotation per pixel of mouse movement. */
static const float SENS_OPTIONS[] = {
    0.0010f, 0.0015f, 0.0020f, 0.0025f, 0.0030f,
    0.0040f, 0.0050f, 0.0065f, 0.0080f, 0.0100f
};
#define SENS_COUNT ((int)(sizeof(SENS_OPTIONS) / sizeof(SENS_OPTIONS[0])))
#define SENS_DEFAULT_INDEX 4

static const char *const DISPLAY_LABELS[] = {
    "Windowed",
    "Borderless Windowed",
    "Fullscreen",
};

static const char *const ONOFF_LABELS[] = { "Off", "On" };

/* ------------------------------ lifecycle -------------------------------- */

void settings_default(Settings *s)
{
    s->fps_index    = FPS_DEFAULT_INDEX;
    s->res_index    = RES_DEFAULT_INDEX;
    s->display_mode = DISPLAY_WINDOWED;
    s->sens_index   = SENS_DEFAULT_INDEX;
    s->cheat_key    = KEY_TAB;
    s->vsync        = false;
}

/* raylib has no key-name function, so here is a small one. Letters and digits
 * already use their ASCII codes, and the function keys are contiguous, which
 * covers most of the keyboard in three lines. */
const char *settings_key_name(int key)
{
    switch (key) {
    case KEY_TAB:           return "Tab";
    case KEY_SPACE:         return "Space";
    case KEY_ENTER:         return "Enter";
    case KEY_BACKSPACE:     return "Backspace";
    case KEY_INSERT:        return "Insert";
    case KEY_DELETE:        return "Delete";
    case KEY_HOME:          return "Home";
    case KEY_END:           return "End";
    case KEY_PAGE_UP:       return "Page Up";
    case KEY_PAGE_DOWN:     return "Page Down";
    case KEY_LEFT:          return "Left";
    case KEY_RIGHT:         return "Right";
    case KEY_UP:            return "Up";
    case KEY_DOWN:          return "Down";
    case KEY_LEFT_SHIFT:    return "L Shift";
    case KEY_RIGHT_SHIFT:   return "R Shift";
    case KEY_LEFT_CONTROL:  return "L Ctrl";
    case KEY_RIGHT_CONTROL: return "R Ctrl";
    case KEY_LEFT_ALT:      return "L Alt";
    case KEY_RIGHT_ALT:     return "R Alt";
    case KEY_CAPS_LOCK:     return "Caps Lock";
    case KEY_GRAVE:         return "`";
    case KEY_MINUS:         return "-";
    case KEY_EQUAL:         return "=";
    case KEY_LEFT_BRACKET:  return "[";
    case KEY_RIGHT_BRACKET: return "]";
    case KEY_BACKSLASH:     return "\\";
    case KEY_SEMICOLON:     return ";";
    case KEY_APOSTROPHE:    return "'";
    case KEY_COMMA:         return ",";
    case KEY_PERIOD:        return ".";
    case KEY_SLASH:         return "/";
    default: break;
    }
    if (key >= KEY_A  && key <= KEY_Z)    return TextFormat("%c", key);
    if (key >= KEY_ZERO && key <= KEY_NINE) return TextFormat("%c", key);
    if (key >= KEY_F1 && key <= KEY_F12)  return TextFormat("F%i", key - KEY_F1 + 1);
    return TextFormat("Key %i", key);
}

int settings_fps_limit(const Settings *s)
{
    return FPS_OPTIONS[s->fps_index];
}

float settings_sensitivity(const Settings *s)
{
    return SENS_OPTIONS[s->sens_index];
}

bool settings_equal(const Settings *a, const Settings *b)
{
    /* Compared field by field rather than with memcmp: struct padding bytes
     * are uninitialised, so memcmp can report a difference that does not
     * exist. */
    return a->fps_index    == b->fps_index
        && a->res_index    == b->res_index
        && a->display_mode == b->display_mode
        && a->sens_index   == b->sens_index
        && a->cheat_key    == b->cheat_key
        && a->vsync        == b->vsync;
}

/* --------------------------- applying to window -------------------------- */

static void apply_fps(const Settings *s)
{
    /* raylib treats a target below 1 as "no limit", which is exactly the
     * behaviour we want for the unlimited stop. */
    SetTargetFPS(FPS_OPTIONS[s->fps_index]);
}

static void apply_vsync(const Settings *s)
{
    if (s->vsync) SetWindowState(FLAG_VSYNC_HINT);
    else          ClearWindowState(FLAG_VSYNC_HINT);
}

static void apply_display(const Settings *s)
{
    int w = RESOLUTIONS[s->res_index][0];
    int h = RESOLUTIONS[s->res_index][1];

    /* Always return to a plain decorated window first, so every transition
     * starts from the same known state instead of stacking flags. */
    if (IsWindowFullscreen())                         ToggleFullscreen();
    if (IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE)) ToggleBorderlessWindowed();

    switch (s->display_mode) {
    case DISPLAY_BORDERLESS:
        /* Resizes itself to the monitor, so the resolution choice is moot. */
        ToggleBorderlessWindowed();
        break;

    case DISPLAY_FULLSCREEN:
        SetWindowSize(w, h);
        ToggleFullscreen();
        break;

    case DISPLAY_WINDOWED:
    default: {
        SetWindowSize(w, h);

        int mon = GetCurrentMonitor();
        int mw = GetMonitorWidth(mon);
        int mh = GetMonitorHeight(mon);

        /* Centre, but never let the title bar go above the top of the screen
         * -- otherwise picking 4K on a 1080p monitor makes the window
         * impossible to grab. */
        int px = (mw - w) / 2;
        int py = (mh - h) / 2;
        if (px < 0) px = 0;
        if (py < 0) py = 0;
        SetWindowPosition(px, py);
        break;
    }
    }
}

void settings_apply_all(const Settings *s)
{
    apply_display(s);
    apply_vsync(s);
    apply_fps(s);
}

/* ------------------------------ persistence ------------------------------ */
/*
 * A save file is just a struct written to disk -- but never write the live
 * struct directly. Its layout can change as the game grows, and padding is
 * compiler-dependent. Instead define an explicit on-disk record of fixed-width
 * types, with a magic number (is this even our file?), a version (do we still
 * understand it?), and a checksum (was it corrupted or hand-edited?).
 */

#define SAVE_MAGIC   0x31465241u   /* "ARF1" little-endian */
#define SAVE_VERSION 2u            /* v2 added cheat_key */

typedef struct SettingsRecord {
    uint32_t magic;
    uint32_t version;
    int32_t  fps_index;
    int32_t  res_index;
    int32_t  display_mode;
    int32_t  sens_index;
    int32_t  vsync;
    int32_t  cheat_key;            /* added in v2 */
    uint32_t checksum;
} SettingsRecord;

/* FNV-1a over every byte before the checksum field. */
static uint32_t record_checksum(const SettingsRecord *r)
{
    const unsigned char *p = (const unsigned char *)r;
    size_t n = offsetof(SettingsRecord, checksum);
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static const char *save_path(void)
{
    /* Next to the executable, so it does not depend on the working directory
     * the game happened to be launched from. */
    return TextFormat("%ssettings.dat", GetApplicationDirectory());
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

bool settings_save(const Settings *s)
{
    SettingsRecord r;
    r.magic        = SAVE_MAGIC;
    r.version      = SAVE_VERSION;
    r.fps_index    = s->fps_index;
    r.res_index    = s->res_index;
    r.display_mode = s->display_mode;
    r.sens_index   = s->sens_index;
    r.vsync        = s->vsync ? 1 : 0;
    r.cheat_key    = s->cheat_key;
    r.checksum     = record_checksum(&r);

    FILE *f = fopen(save_path(), "wb");
    if (!f) return false;

    bool ok = (fwrite(&r, sizeof r, 1, f) == 1);
    fclose(f);
    return ok;
}

bool settings_load(Settings *s)
{
    FILE *f = fopen(save_path(), "rb");
    if (!f) return false;

    SettingsRecord r;
    bool ok = (fread(&r, sizeof r, 1, f) == 1);
    fclose(f);
    if (!ok) return false;

    if (r.magic != SAVE_MAGIC)            return false;
    if (r.version != SAVE_VERSION)        return false;
    if (r.checksum != record_checksum(&r)) return false;

    /* Clamp everything. A file that says res_index = 900 would otherwise walk
     * straight off the end of the table -- the save file is untrusted input
     * exactly like anything else read from disk. */
    s->fps_index    = clampi(r.fps_index,    0, FPS_COUNT - 1);
    s->res_index    = clampi(r.res_index,    0, RES_COUNT - 1);
    s->display_mode = clampi(r.display_mode, 0, DISPLAY_MODE_COUNT - 1);
    s->sens_index   = clampi(r.sens_index,   0, SENS_COUNT - 1);
    s->vsync        = (r.vsync != 0);
    /* A nonsense keycode would leave the cheat menu permanently unreachable,
     * so fall back to the default rather than trusting it. */
    s->cheat_key    = (r.cheat_key > 0 && r.cheat_key < 400) ? r.cheat_key : KEY_TAB;
    return true;
}

/* -------------------------------- menu ----------------------------------- */

static const char *fps_label(int index)
{
    int v = FPS_OPTIONS[index];
    return (v == 0) ? "Unlimited" : TextFormat("%i FPS", v);
}

/* True while the rebind row is swallowing the next keypress. */
static bool g_capturing;

bool settings_is_rebinding(void)
{
    return g_capturing;
}

/* Dim the game and lay out a centred panel with a heading. Every page starts
 * this way, so they line up with each other instead of drifting apart. */
static Rectangle panel_begin(const char *title, float pw, float ph)
{
    ui_begin();

    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Fade(BLACK, 0.6f));

    Rectangle panel = { (GetScreenWidth()  - pw) * 0.5f,
                        (GetScreenHeight() - ph) * 0.5f, pw, ph };

    DrawRectangleRounded(panel, 0.04f, 8, UI_BG);
    DrawRectangleRoundedLines(panel, 0.04f, 8, UI_BORDER);

    ui_text_center((Rectangle){ panel.x, panel.y + 18.0f, panel.width, 34.0f },
                   28, title, UI_TEXT);
    return panel;
}

/* ------------------------------ pause menu ------------------------------- */

MenuAction pause_menu(void)
{
    MenuAction action = MENU_NONE;

    Rectangle panel = panel_begin("PAUSED", 340.0f, 412.0f);

    const float pad = 30.0f;
    const float bw  = panel.width - pad * 2.0f;
    float y = panel.y + 76.0f;

    if (ui_button((Rectangle){ panel.x + pad, y, bw, 46.0f }, "Resume", false, true))
        action = MENU_RESUME;
    y += 60.0f;

    if (ui_button((Rectangle){ panel.x + pad, y, bw, 46.0f }, "Settings", false, true))
        action = MENU_OPEN_SETTINGS;
    y += 60.0f;

    if (ui_button((Rectangle){ panel.x + pad, y, bw, 46.0f }, "Keybinds", false, true))
        action = MENU_OPEN_KEYBINDS;
    y += 60.0f;

    /* Abandons the run. Harmless today because there is nothing to lose yet --
     * once a match has a score or a health bar worth keeping, this is where a
     * confirmation step belongs. */
    if (ui_button((Rectangle){ panel.x + pad, y, bw, 46.0f }, "Main Menu", false, true))
        action = MENU_MAIN_MENU;
    y += 60.0f;

    if (ui_button((Rectangle){ panel.x + pad, y, bw, 46.0f }, "Quit Game", true, true))
        action = MENU_QUIT;

    ui_text_center((Rectangle){ panel.x, panel.y + panel.height - 30.0f,
                                panel.width, 16.0f }, 14,
                   "Esc resumes", Fade(UI_MUTED, 0.8f));
    return action;
}

/* ------------------------------- settings -------------------------------- */

MenuAction settings_menu(Settings *pending, const Settings *applied)
{
    MenuAction action = MENU_NONE;
    bool dirty = !settings_equal(pending, applied);

    Rectangle panel = panel_begin("SETTINGS", 580.0f, 500.0f);

    const float pad   = 34.0f;
    const float lbl_w = 150.0f;
    const float ctl_x = panel.x + pad + lbl_w;
    const float ctl_w = panel.width - pad * 2.0f - lbl_w;
    float y = panel.y + 76.0f;
    const float row = 52.0f;

    /* --- frame rate limit ------------------------------------------------ */
    ui_text((int)(panel.x + pad), (int)(y + 6), 18, "Frame Limit", UI_MUTED);
    {
        Rectangle sr = { ctl_x, y, ctl_w - 110.0f, 28.0f };
        ui_slider_index(sr, &pending->fps_index, FPS_COUNT, true);
        Rectangle vr = { ctl_x + ctl_w - 100.0f, y, 100.0f, 28.0f };
        ui_text_center(vr, 18, fps_label(pending->fps_index), UI_ACCENT);
    }
    y += row;

    /* --- mouse sensitivity ----------------------------------------------- */
    ui_text((int)(panel.x + pad), (int)(y + 6), 18, "Look Sensitivity", UI_MUTED);
    {
        Rectangle sr = { ctl_x, y, ctl_w - 110.0f, 28.0f };
        ui_slider_index(sr, &pending->sens_index, SENS_COUNT, true);
        Rectangle vr = { ctl_x + ctl_w - 100.0f, y, 100.0f, 28.0f };
        ui_text_center(vr, 18, TextFormat("%i / %i", pending->sens_index + 1,
                                          SENS_COUNT), UI_ACCENT);
    }
    y += row;

    /* --- vsync ----------------------------------------------------------- */
    ui_text((int)(panel.x + pad), (int)(y + 8), 18, "V-Sync", UI_MUTED);
    {
        Rectangle cr = { ctl_x, y, ctl_w, 34.0f };
        int idx = pending->vsync ? 1 : 0;
        if (ui_cycler(cr, &idx, 2, ONOFF_LABELS, true)) pending->vsync = (idx != 0);
    }
    y += row;

    /* --- display mode ---------------------------------------------------- */
    ui_text((int)(panel.x + pad), (int)(y + 8), 18, "Display", UI_MUTED);
    {
        Rectangle cr = { ctl_x, y, ctl_w, 34.0f };
        ui_cycler(cr, &pending->display_mode, DISPLAY_MODE_COUNT,
                  DISPLAY_LABELS, true);
    }
    y += row;

    /* --- resolution ------------------------------------------------------ */
    {
        bool res_enabled = (pending->display_mode != DISPLAY_BORDERLESS);
        ui_text((int)(panel.x + pad), (int)(y + 8), 18, "Resolution",
                res_enabled ? UI_MUTED : Fade(UI_MUTED, 0.45f));

        Rectangle cr = { ctl_x, y, ctl_w, 34.0f };
        ui_cycler(cr, &pending->res_index, RES_COUNT, RES_LABELS, res_enabled);
        y += 38.0f;

        if (!res_enabled) {
            ui_text((int)ctl_x, (int)y, 14,
                    "Borderless always matches the monitor", Fade(UI_MUTED, 0.7f));
        } else {
            int mon = GetCurrentMonitor();
            ui_text((int)ctl_x, (int)y, 14,
                    TextFormat("Monitor: %i x %i", GetMonitorWidth(mon),
                               GetMonitorHeight(mon)), Fade(UI_MUTED, 0.7f));
        }
    }
    /* --- buttons --------------------------------------------------------- */
    {
        float bw = (panel.width - pad * 2.0f - 16.0f) * 0.5f;
        float by = panel.y + panel.height - 124.0f;

        Rectangle apply    = { panel.x + pad,              by, bw, 44.0f };
        Rectangle defaults = { panel.x + pad + bw + 16.0f, by, bw, 44.0f };

        if (ui_button(apply, dirty ? "Apply Now" : "Applied", false, dirty))
            action = MENU_APPLY;
        if (ui_button(defaults, "Restore Defaults", false, true))
            action = MENU_DEFAULTS;

        by += 54.0f;
        Rectangle back = { panel.x + pad, by, panel.width - pad * 2.0f, 44.0f };
        if (ui_button(back, "Back", false, true)) action = MENU_BACK;
    }

    ui_text_center((Rectangle){ panel.x, panel.y + panel.height - 22.0f,
                                panel.width, 16.0f }, 14,
                   dirty ? "Changes apply and save when you go back"
                         : "Saved -- these settings survive a restart",
                   dirty ? Fade(UI_ACCENT, 0.95f) : Fade(UI_MUTED, 0.8f));

    return action;
}

/* ------------------------------- keybinds -------------------------------- */

/* One rebindable row: a label on the left and a button showing the current key
 * that turns into a capture box when clicked. */
static void keybind_row(Rectangle r, const char *label, int *key,
                        float label_x, bool *capturing)
{
    ui_text((int)label_x, (int)(r.y + 8), 18, label, UI_MUTED);

    if (*capturing) {
        DrawRectangleRounded(r, 0.28f, 6, Fade(UI_ACCENT, 0.22f));
        DrawRectangleRoundedLines(r, 0.28f, 6, UI_ACCENT);
        ui_text_center(r, 18, "Press a key...   (Esc cancels)", UI_ACCENT);

        /* GetKeyPressed drains a queue, so this reliably catches the key even
         * on a frame where several arrived at once. */
        int k = GetKeyPressed();
        if (k != 0) {
            if (k == KEY_ESCAPE) *capturing = false;    /* cancel */
            else { *key = k; *capturing = false; }
        }
    } else {
        if (ui_button(r, settings_key_name(*key), false, true)) *capturing = true;
    }
}

MenuAction keybinds_menu(Settings *pending, const Settings *applied)
{
    MenuAction action = MENU_NONE;
    bool dirty = !settings_equal(pending, applied);

    Rectangle panel = panel_begin("KEYBINDS", 520.0f, 300.0f);

    const float pad   = 32.0f;
    const float lbl_w = 170.0f;
    const float ctl_x = panel.x + pad + lbl_w;
    const float ctl_w = panel.width - pad * 2.0f - lbl_w;
    float y = panel.y + 84.0f;

    keybind_row((Rectangle){ ctl_x, y, ctl_w, 34.0f }, "Cheat Menu",
                &pending->cheat_key, panel.x + pad, &g_capturing);
    y += 44.0f;

    ui_text((int)(panel.x + pad), (int)y, 14,
            "Movement is fixed to WASD, Space and Shift for now.",
            Fade(UI_MUTED, 0.75f));

    /* --- back ------------------------------------------------------------ */
    {
        Rectangle back = { panel.x + pad, panel.y + panel.height - 92.0f,
                           panel.width - pad * 2.0f, 44.0f };
        if (ui_button(back, "Back", false, true)) action = MENU_BACK;
    }

    ui_text_center((Rectangle){ panel.x, panel.y + panel.height - 30.0f,
                                panel.width, 16.0f }, 14,
                   dirty ? "Changes apply and save when you go back"
                         : "Saved -- this binding survives a restart",
                   dirty ? Fade(UI_ACCENT, 0.95f) : Fade(UI_MUTED, 0.8f));

    /* Never leave a capture hanging once the page is being dismissed. */
    if (action != MENU_NONE) g_capturing = false;

    return action;
}
