#include "cheats.h"
#include "ui.h"

#include "raylib.h"

static const char *const ONOFF_LABELS[] = { "Off", "On" };

void cheats_default(Cheats *c)
{
    c->unlimited_sprint = false;
}

bool cheats_menu(Cheats *c)
{
    bool close = false;

    ui_begin();

    int sw = GetScreenWidth();
    int sh = GetScreenHeight();

    DrawRectangle(0, 0, sw, sh, Fade(BLACK, 0.6f));

    const float pw = 520.0f, ph = 260.0f;
    Rectangle panel = { (sw - pw) * 0.5f, (sh - ph) * 0.5f, pw, ph };

    DrawRectangleRounded(panel, 0.05f, 8, UI_BG);
    /* Amber border so it never gets mistaken for the normal options panel. */
    DrawRectangleRoundedLines(panel, 0.05f, 8, (Color){ 235, 203, 139, 255 });

    ui_text_center((Rectangle){ panel.x, panel.y + 16.0f, panel.width, 32.0f },
                   26, "CHEATS", (Color){ 235, 203, 139, 255 });

    const float pad   = 32.0f;
    const float lbl_w = 190.0f;
    const float ctl_x = panel.x + pad + lbl_w;
    const float ctl_w = panel.width - pad * 2.0f - lbl_w;
    float y = panel.y + 74.0f;

    /* --- unlimited sprint ------------------------------------------------ */
    ui_text((int)(panel.x + pad), (int)(y + 8), 18, "Unlimited Sprint", UI_MUTED);
    {
        Rectangle cr = { ctl_x, y, ctl_w, 34.0f };
        int idx = c->unlimited_sprint ? 1 : 0;
        if (ui_cycler(cr, &idx, 2, ONOFF_LABELS, true))
            c->unlimited_sprint = (idx != 0);
    }
    y += 44.0f;

    ui_text((int)(panel.x + pad), (int)y, 14,
            "Stamina never drains and exhaustion never triggers.",
            Fade(UI_MUTED, 0.75f));

    /* --- close ----------------------------------------------------------- */
    {
        Rectangle close_btn = { panel.x + pad, panel.y + panel.height - 66.0f,
                                panel.width - pad * 2.0f, 44.0f };
        if (ui_button(close_btn, "Close", false, true)) close = true;
    }

    return close;
}
