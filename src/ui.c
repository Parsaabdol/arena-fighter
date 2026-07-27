#include "ui.h"

#include <math.h>

/*
 * The single piece of state an immediate-mode UI genuinely needs: which
 * control currently owns the mouse. Without this, dragging a slider and
 * moving the cursor off it would drop the drag.
 *
 * We use the address of the caller's own variable as the identity, which is
 * unique for free -- no manual widget IDs to keep in sync.
 */
static const void *g_active;

void ui_begin(void)
{
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) g_active = NULL;
}

void ui_text(int x, int y, int size, const char *s, Color c)
{
    DrawText(s, x, y, size, c);
}

void ui_text_center(Rectangle r, int size, const char *s, Color c)
{
    int w = MeasureText(s, size);
    DrawText(s, (int)(r.x + (r.width - w) * 0.5f),
                (int)(r.y + (r.height - size) * 0.5f), size, c);
}

/* ------------------------------- button ---------------------------------- */

bool ui_button(Rectangle r, const char *label, bool danger, bool enabled)
{
    Vector2 m = GetMousePosition();
    bool hover = enabled && CheckCollisionPointRec(m, r);
    bool held  = hover && IsMouseButtonDown(MOUSE_BUTTON_LEFT);

    Color base = danger ? UI_DANGER : UI_ACCENT;

    if (!enabled) {
        DrawRectangleRounded(r, 0.25f, 6, Fade(UI_MUTED, 0.08f));
        DrawRectangleRoundedLines(r, 0.25f, 6, Fade(UI_BORDER, 0.45f));
        ui_text_center(r, 18, label, Fade(UI_MUTED, 0.45f));
        return false;
    }

    Color fill = held  ? Fade(base, 0.55f)
               : hover ? Fade(base, 0.32f)
                       : Fade(base, 0.14f);

    DrawRectangleRounded(r, 0.25f, 6, fill);
    DrawRectangleRoundedLines(r, 0.25f, 6, hover ? base : UI_BORDER);
    ui_text_center(r, 18, label, hover ? UI_TEXT : Fade(UI_TEXT, 0.85f));

    return hover && IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
}

/* ------------------------------- slider ---------------------------------- */

bool ui_slider_index(Rectangle r, int *index, int count, bool enabled)
{
    if (count < 1) return false;

    Vector2 m = GetMousePosition();
    bool hover = enabled && CheckCollisionPointRec(m, r);
    bool changed = false;

    if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) g_active = index;

    if (enabled && g_active == index && count > 1) {
        /* Map cursor X across the track to the nearest discrete stop. */
        float t = (m.x - r.x) / r.width;
        t = fmaxf(0.0f, fminf(1.0f, t));
        int ni = (int)(t * (float)(count - 1) + 0.5f);
        if (ni != *index) { *index = ni; changed = true; }
    }

    /* Arrow keys nudge while hovering -- cheap precision for a coarse slider. */
    if (hover) {
        if (IsKeyPressed(KEY_LEFT)  && *index > 0)         { (*index)--; changed = true; }
        if (IsKeyPressed(KEY_RIGHT) && *index < count - 1) { (*index)++; changed = true; }
    }

    float t = (count > 1) ? (float)*index / (float)(count - 1) : 0.0f;
    float cy = r.y + r.height * 0.5f;

    Rectangle track = { r.x, cy - 3.0f, r.width, 6.0f };
    DrawRectangleRounded(track, 1.0f, 4, Fade(BLACK, enabled ? 0.45f : 0.25f));

    Rectangle done = { r.x, cy - 3.0f, r.width * t, 6.0f };
    DrawRectangleRounded(done, 1.0f, 4,
                         enabled ? UI_ACCENT : Fade(UI_MUTED, 0.4f));

    float knob = (hover || g_active == index) ? 9.0f : 7.0f;
    DrawCircle((int)(r.x + r.width * t), (int)cy, knob,
               enabled ? UI_TEXT : Fade(UI_MUTED, 0.5f));

    return changed;
}

/* ------------------------------- cycler ---------------------------------- */

bool ui_cycler(Rectangle r, int *index, int count,
               const char *const *labels, bool enabled)
{
    bool changed = false;
    float bw = 30.0f;

    Rectangle lb = { r.x, r.y, bw, r.height };
    Rectangle rb = { r.x + r.width - bw, r.y, bw, r.height };
    Rectangle mid = { r.x + bw, r.y, r.width - bw * 2.0f, r.height };

    Vector2 m = GetMousePosition();
    bool lh = enabled && CheckCollisionPointRec(m, lb);
    bool rh = enabled && CheckCollisionPointRec(m, rb);

    DrawRectangleRounded(r, 0.28f, 6, Fade(BLACK, enabled ? 0.35f : 0.18f));
    DrawRectangleRoundedLines(r, 0.28f, 6, Fade(UI_BORDER, enabled ? 1.0f : 0.4f));

    ui_text_center(lb, 20, "<", lh ? UI_ACCENT : Fade(UI_MUTED, enabled ? 1.0f : 0.4f));
    ui_text_center(rb, 20, ">", rh ? UI_ACCENT : Fade(UI_MUTED, enabled ? 1.0f : 0.4f));
    ui_text_center(mid, 18, labels[*index],
                   enabled ? UI_TEXT : Fade(UI_MUTED, 0.55f));

    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        if (lh) { *index = (*index - 1 + count) % count; changed = true; }
        if (rh) { *index = (*index + 1) % count;         changed = true; }
    }
    return changed;
}
