#include "frontend.h"
#include "game.h"     /* the inspection camera */
#include "model.h"
#include "ui.h"

#include "raylib.h"

#include <stdio.h>    /* snprintf */

/* ---------------------------------------------------------------------------
 * Shared title treatment.
 *
 * raylib's built-in font is a 10-pixel bitmap, so scaling it to title size gives
 * chunky letterforms. Rather than fight that with an asset, the treatment leans
 * into it: uppercase, wide letter spacing, a soft drop shadow and a thin accent
 * rule. Spaced out deliberately, blocky reads as a design choice instead of as a
 * scaled-up default -- and it costs no font file to ship.
 * ------------------------------------------------------------------------- */

#define TITLE_SIZE        54.0f
#define TITLE_TRACK        7.0f   /* settled letter spacing, in pixels        */
#define TITLE_TRACK_WIDE  30.0f   /* where the intro's tracking-in starts     */
#define TITLE_SHADOW       3.0f   /* drop shadow offset                       */

#define RULE_GAP          20.0f   /* title baseline area -> accent rule       */
#define RULE_OVERHANG     56.0f   /* how far the rule runs past the title     */
#define RULE_THICK         2

#define TAG_SIZE          16.0f
#define TAG_TRACK          5.0f
#define TAG_GAP           30.0f   /* rule -> tagline                          */

/* How dark the world goes behind the menu. Enough that white text is legible
 * over the arena floor at any camera angle, little enough that you can still
 * see the fighter breathing back there. */
#define FE_SCRIM           0.62f

/* Height of the title block, as a fraction of the screen. The intro and the
 * menu deliberately share it: the two draw the same words at the same size in
 * the same place, so when the card fades off the title does not move at all.
 * That stillness is what makes the handoff read as one shot -- change this and
 * you get a visible jump between the two screens. */
#define FE_TITLE_Y         0.34f

static float clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/* Normalised progress through the window [a, b] of a timeline. */
static float ramp(float t, float a, float b)
{
    return (b <= a) ? 1.0f : clamp01((t - a) / (b - a));
}

static float ease_out(float t)
{
    float u = 1.0f - t;
    return 1.0f - u * u * u;
}

static float tracked_width(const char *s, float size, float tracking)
{
    return MeasureTextEx(GetFontDefault(), s, size, tracking).x;
}

/* Draw text centred on (cx, cy) with explicit letter spacing. Returns the drawn
 * width, so a caller can size a rule to match the text above it. */
static float draw_tracked(const char *s, float cx, float cy, float size,
                          float tracking, Color c)
{
    Font f = GetFontDefault();
    Vector2 m = MeasureTextEx(f, s, size, tracking);
    Vector2 p = { cx - m.x * 0.5f, cy - m.y * 0.5f };

    /* Shadow alpha tracks the text's own, so the pair fades as one thing. */
    DrawTextEx(f, s, (Vector2){ p.x + TITLE_SHADOW, p.y + TITLE_SHADOW },
               size, tracking, Fade(BLACK, ((float)c.a / 255.0f) * 0.55f));
    DrawTextEx(f, s, p, size, tracking, c);

    return m.x;
}

/* Scrim plus a top/bottom falloff. The gradient bands are what let the text
 * stay legible over any part of the arena without dimming the middle of the
 * frame, where the fighter is. */
static void fe_backdrop(void)
{
    int sw = GetScreenWidth();
    int sh = GetScreenHeight();
    int band = sh / 4;

    DrawRectangle(0, 0, sw, sh, Fade(BLACK, FE_SCRIM));
    DrawRectangleGradientV(0, 0, sw, band, Fade(BLACK, 0.55f), BLANK);
    DrawRectangleGradientV(0, sh - band, sw, band, BLANK, Fade(BLACK, 0.65f));
}

/* ---------------------------------------------------------------------------
 * Intro
 *
 * Timeline as absolute stamps rather than durations, because the beats overlap
 * on purpose (the rule is still wiping out while the title starts arriving) and
 * overlapping durations are much harder to read than overlapping stamps.
 * ------------------------------------------------------------------------- */

#define T_RULE_START   0.35f
#define T_RULE_FULL    1.30f
#define T_TITLE_START  0.95f
#define T_TITLE_FULL   2.20f
#define T_TAG_START    1.85f
#define T_TAG_FULL     2.60f
#define T_FADE_START   3.55f
#define T_END          4.35f

#define INTRO_TAGLINE  "STEP INTO THE ARENA"

static float g_intro_t;
static bool  g_intro_skipped;

void intro_reset(void)
{
    g_intro_t = 0.0f;
    g_intro_skipped = false;
}

bool intro_draw(float dt)
{
    /* A skip jumps to the start of the fade instead of ending the sequence
     * outright. Two reasons: the handoff to the menu stays smooth, and the
     * mouse RELEASE that follows a skip-click gets consumed here rather than
     * landing on a menu button that appeared underneath it. */
    if (!g_intro_skipped &&
        (GetKeyPressed() != 0 || IsMouseButtonPressed(MOUSE_BUTTON_LEFT))) {
        g_intro_skipped = true;
        if (g_intro_t < T_FADE_START) g_intro_t = T_FADE_START;
    }

    g_intro_t += dt;

    int sw = GetScreenWidth();
    int sh = GetScreenHeight();
    float cx = (float)sw * 0.5f;
    float cy = (float)sh * FE_TITLE_Y;

    /* The card is opaque black for most of its life and lifts away completely at
     * the end. Whatever is drawn behind it -- the main menu, over the live arena
     * -- is therefore what it dissolves into, so the two screens read as one
     * continuous shot instead of a cut. */
    float fade  = ease_out(ramp(g_intro_t, T_FADE_START, T_END));
    float leave = 1.0f - fade;

    DrawRectangle(0, 0, sw, sh, Fade(BLACK, leave));

    /* --- title: tracking eases in from wide, which reads as the letters
     *     settling into place rather than as a plain fade ------------------- */
    float ti = ease_out(ramp(g_intro_t, T_TITLE_START, T_TITLE_FULL));
    float tracking = TITLE_TRACK_WIDE + (TITLE_TRACK - TITLE_TRACK_WIDE) * ti;
    draw_tracked(GAME_TITLE_CAPS, cx, cy, TITLE_SIZE, tracking,
                 Fade(UI_TEXT, ti * leave));

    /* --- accent rule wiping out from the centre ------------------------- */
    /* Sized from the SETTLED title width, not the animated one: a rule that
     * followed the tracking would grow and then shrink again. */
    float ri = ease_out(ramp(g_intro_t, T_RULE_START, T_RULE_FULL));
    float rw = (tracked_width(GAME_TITLE_CAPS, TITLE_SIZE, TITLE_TRACK)
                + RULE_OVERHANG) * ri;
    float ry = cy + TITLE_SIZE * 0.5f + RULE_GAP;

    DrawRectangle((int)(cx - rw * 0.5f), (int)ry, (int)rw, RULE_THICK,
                  Fade(UI_ACCENT, ri * leave));

    /* --- tagline --------------------------------------------------------- */
    float gi = ease_out(ramp(g_intro_t, T_TAG_START, T_TAG_FULL));
    draw_tracked(INTRO_TAGLINE, cx, ry + TAG_GAP, TAG_SIZE, TAG_TRACK,
                 Fade(UI_MUTED, gi * leave));

    /* --- skip hint: offered only until it is taken ------------------------ */
    if (!g_intro_skipped) {
        const char *hint = "press any key to skip";
        float hi = ramp(g_intro_t, T_TAG_START, T_TAG_START + 0.6f);
        ui_text(sw - MeasureText(hint, 14) - 24, sh - 34, 14, hint,
                Fade(UI_MUTED, 0.55f * hi * leave));
    }

    return g_intro_t >= T_END;
}

/* ---------------------------------------------------------------------------
 * Main menu
 *
 * Deliberately not a panel. The pause menu is a box because it sits on top of a
 * game in progress; the main menu owns the whole frame, so it is just a title
 * block and a column of rows over the arena.
 * ------------------------------------------------------------------------- */

#define MM_BTN_W    280.0f
#define MM_BTN_H     48.0f
#define MM_BTN_GAP   12.0f
#define MM_COL_Y      0.50f   /* fraction of screen height */
#define MM_COL_CLEAR 68.0f    /* minimum gap below the rule on a short window */

MenuAction main_menu(void)
{
    MenuAction action = MENU_NONE;

    ui_begin();

    int sw = GetScreenWidth();
    int sh = GetScreenHeight();
    float cx = (float)sw * 0.5f;

    fe_backdrop();

    /* --- title block: same words, size and place as the intro card ------- */
    float ty = (float)sh * FE_TITLE_Y;
    float tw = draw_tracked(GAME_TITLE_CAPS, cx, ty, TITLE_SIZE, TITLE_TRACK,
                            UI_TEXT);

    float ry = ty + TITLE_SIZE * 0.5f + RULE_GAP;
    float rw = tw + RULE_OVERHANG;
    DrawRectangle((int)(cx - rw * 0.5f), (int)ry, (int)rw, RULE_THICK, UI_ACCENT);

    /* --- rows ------------------------------------------------------------ */
    float by = (float)sh * MM_COL_Y;
    if (by < ry + MM_COL_CLEAR) by = ry + MM_COL_CLEAR;

    Rectangle r = { cx - MM_BTN_W * 0.5f, by, MM_BTN_W, MM_BTN_H };

    /* Play is the primary action, so it gets a filled backdrop the other rows
     * do not have. ui_button's own fill is translucent and layers over this,
     * which is cheaper than teaching the widget about variants. */
    DrawRectangleRounded(r, 0.25f, 6, Fade(UI_ACCENT, 0.18f));
    if (ui_button(r, "Play", false, true) || IsKeyPressed(KEY_ENTER))
        action = MENU_PLAY;
    r.y += MM_BTN_H + MM_BTN_GAP;

    if (ui_button(r, "Customize", false, true)) action = MENU_OPEN_CUSTOMIZE;
    r.y += MM_BTN_H + MM_BTN_GAP;

    if (ui_button(r, "Settings", false, true)) action = MENU_OPEN_SETTINGS;
    r.y += MM_BTN_H + MM_BTN_GAP;

    if (ui_button(r, "Keybinds", false, true)) action = MENU_OPEN_KEYBINDS;
    r.y += MM_BTN_H + MM_BTN_GAP;

    if (ui_button(r, "Quit", true, true)) action = MENU_QUIT;

    /* --- footer ---------------------------------------------------------- */
    ui_text(24, sh - 34, 14, GAME_TITLE "  " GAME_VERSION, Fade(UI_MUTED, 0.7f));

    const char *hint = "Enter to play";
    ui_text(sw - MeasureText(hint, 14) - 24, sh - 34, 14, hint,
            Fade(UI_MUTED, 0.55f));

    return action;
}

/* ---------------------------------------------------------------------------
 * Customize
 *
 * The panel sits well left of centre on purpose: the front-end camera parks the
 * fighter on the right of the frame, so leaving that side clear turns the whole
 * screen into the preview. No second viewport, no second copy of the character
 * -- you are looking at the real one, under the real camera, which you can
 * drag to orbit and scroll to zoom.
 * ------------------------------------------------------------------------- */

#define CZ_PANEL_W   430.0f
#define CZ_INSPECT_SENS 0.006f   /* radians per pixel dragged */

/* Panel chrome shared by both appearance pages, so the one that is one level
 * down looks like the same place rather than a different screen. */
static Rectangle appearance_panel(const char *title, float ph)
{
    int sw = GetScreenWidth();
    int sh = GetScreenHeight();

    /* Lighter than the settings dim: the whole point here is to see him. */
    DrawRectangle(0, 0, sw, sh, Fade(BLACK, 0.45f));

    float px = (float)sw * 0.06f;
    if (px + CZ_PANEL_W > (float)sw - 24.0f) px = 24.0f;   /* narrow window */

    Rectangle panel = { px, ((float)sh - ph) * 0.5f, CZ_PANEL_W, ph };
    DrawRectangleRounded(panel, 0.04f, 8, UI_BG);
    DrawRectangleRoundedLines(panel, 0.04f, 8, UI_BORDER);
    ui_text_center((Rectangle){ panel.x, panel.y + 18.0f, panel.width, 34.0f },
                   26, title, UI_TEXT);
    return panel;
}

/*
 * Let the player turn the character over. Dragging anywhere OUTSIDE the panel
 * orbits and the wheel zooms -- outside, so a drag can never be mistaken for a
 * slider, and so the whole empty half of the screen is a handle.
 */
static void inspect_camera(Rectangle panel)
{
    Vector2 m = GetMousePosition();
    bool over_panel = CheckCollisionPointRec(m, panel);

    float dx = 0.0f, dy = 0.0f, wheel = 0.0f;
    if (!over_panel) {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            Vector2 d = GetMouseDelta();
            dx = d.x;
            dy = d.y;
        }
        wheel = GetMouseWheelMove();
    }

    /* Called every frame the page is open, moving or not: the zoom is held
     * only while it is asked for, and eases back out when it is not. */
    render_camera_inspect(dx, dy, wheel, CZ_INSPECT_SENS);
}

static void appearance_footer(const Hero *pending, const Hero *applied)
{
    int sw = GetScreenWidth();
    int sh = GetScreenHeight();

    ui_text(24, sh - 34, 14, "Drag to turn him around   ·   Scroll to zoom",
            Fade(UI_MUTED, 0.6f));

    if (!hero_equal(pending, applied)) {
        const char *s = "Saved when you go back";
        ui_text(sw - MeasureText(s, 14) - 24, sh - 34, 14, s,
                Fade(UI_ACCENT, 0.95f));
    }
}

MenuAction customize_menu(Hero *pending, const Hero *applied)
{
    MenuAction action = MENU_NONE;

    ui_begin();

    Rectangle panel = appearance_panel("CUSTOMIZE", 340.0f);
    inspect_camera(panel);

    const float pad   = 28.0f;
    const float lbl_w = 100.0f;
    const float ctl_x = panel.x + pad + lbl_w;
    const float ctl_w = panel.width - pad * 2.0f - lbl_w;
    float y = panel.y + 76.0f;

    /* An imported mesh brings its own materials, so only the overall tint has
     * anywhere to land -- the two-colour scheme is the built-in model's. */
    bool mesh = (pending->model[0] != '\0') && model_is_mesh();

    ui_text((int)(panel.x + pad), (int)(y + 8), 18, "Colour", UI_MUTED);
    {
        Rectangle cr = { ctl_x, y, ctl_w - 46.0f, 34.0f };
        ui_cycler(cr, &pending->body_index, hero_color_count(),
                  hero_color_names(), true);
        /* A swatch, because a colour name is a poor way to pick a colour. */
        Rectangle sr = { ctl_x + ctl_w - 36.0f, y + 3.0f, 28.0f, 28.0f };
        DrawRectangleRounded(sr, 0.3f, 6, hero_color(pending->body_index));
        DrawRectangleRoundedLines(sr, 0.3f, 6, UI_BORDER);
    }
    y += 46.0f;

    ui_text((int)(panel.x + pad), (int)(y + 8), 18, "Accent",
            mesh ? Fade(UI_MUTED, 0.45f) : UI_MUTED);
    {
        Rectangle cr = { ctl_x, y, ctl_w - 46.0f, 34.0f };
        ui_cycler(cr, &pending->accent_index, hero_color_count(),
                  hero_color_names(), !mesh);
        Rectangle sr = { ctl_x + ctl_w - 36.0f, y + 3.0f, 28.0f, 28.0f };
        DrawRectangleRounded(sr, 0.3f, 6,
                             Fade(hero_color(pending->accent_index),
                                  mesh ? 0.35f : 1.0f));
        DrawRectangleRoundedLines(sr, 0.3f, 6, UI_BORDER);
    }
    y += 40.0f;

    ui_text((int)ctl_x, (int)y, 14,
            mesh ? "Imported skins are tinted by Colour alone"
                 : "The built-in fighter uses both",
            Fade(UI_MUTED, 0.7f));
    y += 30.0f;

    /* --- the way one level down ------------------------------------------ */
    {
        int installed = model_count() - 1;      /* entry 0 is the built-in */
        const char *current = (pending->model[0] == '\0')
                            ? "Built-in blocks"
                            : model_label(model_index_of(pending->model));

        Rectangle br = { panel.x + pad, y, panel.width - pad * 2.0f, 42.0f };
        if (ui_button(br, "Mod Skins", false, true)) action = MENU_OPEN_SKINS;
        y += 46.0f;

        ui_text((int)(panel.x + pad), (int)y, 14,
                TextFormat("%s   ·   %i installed",
                           (pending->model[0] && model_index_of(pending->model) < 0)
                               ? pending->model : current,
                           installed),
                Fade(UI_MUTED, 0.7f));
    }

    /* --- buttons --------------------------------------------------------- */
    {
        float bw = (panel.width - pad * 2.0f - 14.0f) * 0.5f;
        float by = panel.y + panel.height - 58.0f;

        if (ui_button((Rectangle){ panel.x + pad, by, bw, 40.0f },
                      "Restore Defaults", false, true))
            hero_default(pending);

        if (ui_button((Rectangle){ panel.x + pad + bw + 14.0f, by, bw, 40.0f },
                      "Back", false, true))
            action = MENU_BACK;
    }

    appearance_footer(pending, applied);
    return action;
}

/* ------------------------------- mod skins -------------------------------- */

#define MS_ROWS_MAX  10          /* rows the panel has room for */
#define MS_ROW_H     36.0f
#define MS_ROW_GAP    6.0f

MenuAction modskins_menu(Hero *pending, const Hero *applied)
{
    MenuAction action = MENU_NONE;

    ui_begin();

    int total = model_count();
    int n = (total > MS_ROWS_MAX) ? MS_ROWS_MAX : total;

    float ph = 150.0f + (float)n * (MS_ROW_H + MS_ROW_GAP);
    Rectangle panel = appearance_panel("MOD SKINS", ph);
    inspect_camera(panel);

    const float pad = 28.0f;
    float y = panel.y + 66.0f;

    int selected = model_index_of(pending->model);

    for (int i = 0; i < n; i++) {
        Rectangle r = { panel.x + pad, y, panel.width - pad * 2.0f, MS_ROW_H };

        /* The active skin gets a filled backdrop and says so; ui_button's own
         * fill is translucent and layers over it, which is cheaper than
         * teaching the widget about a selected state. It stays ENABLED --
         * greying out the row you are using reads as "unavailable" rather than
         * "current", and clicking it again is harmless. */
        bool active = (i == selected);
        if (active) DrawRectangleRounded(r, 0.25f, 6, Fade(UI_ACCENT, 0.22f));

        const char *label = active ? TextFormat("%s   (active)", model_label(i))
                                   : model_label(i);
        if (ui_button(r, label, false, true))
            snprintf(pending->model, HERO_NAME_MAX, "%s", model_file(i));

        y += MS_ROW_H + MS_ROW_GAP;
    }

    y += 4.0f;

    if (total == 1) {
        ui_text((int)(panel.x + pad), (int)y, 14,
                "Nothing installed yet -- drop a .glb in assets/",
                Fade(UI_MUTED, 0.75f));
    } else if (total > MS_ROWS_MAX) {
        ui_text((int)(panel.x + pad), (int)y, 14,
                TextFormat("Showing %i of %i -- the rest are ignored for now",
                           n, total),
                Fade(UI_MUTED, 0.75f));
    } else if (selected < 0) {
        ui_text((int)(panel.x + pad), (int)y, 14,
                TextFormat("\"%s\" is missing; the built-in is drawn",
                           pending->model),
                Fade(UI_DANGER, 0.95f));
    } else if (pending->model[0] && !model_is_mesh()) {
        ui_text((int)(panel.x + pad), (int)y, 14,
                "That file could not be loaded", Fade(UI_DANGER, 0.95f));
    } else if (model_is_mesh()) {
        /* Loading and animating are different kinds of success, and an export
         * can easily manage only the first -- so say which one happened. */
        int clips = model_clip_count();
        if (clips == 0) {
            ui_text((int)(panel.x + pad), (int)y, 14,
                    "No animations in this file -- it will stand still",
                    Fade(UI_DANGER, 0.95f));
        } else {
            ui_text((int)(panel.x + pad), (int)y, 14,
                    TextFormat("%i clips   ·   %i of %i states matched by name",
                               clips, model_matched_count(), (int)ANIM_COUNT),
                    Fade(UI_MUTED, 0.75f));
        }
    }

    {
        Rectangle br = { panel.x + pad, panel.y + panel.height - 56.0f,
                         panel.width - pad * 2.0f, 40.0f };
        if (ui_button(br, "Back", false, true)) action = MENU_BACK;
    }

    appearance_footer(pending, applied);
    return action;
}
