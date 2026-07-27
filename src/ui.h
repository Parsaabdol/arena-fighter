#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include "raylib.h"

/*
 * A tiny immediate-mode UI.
 *
 * "Immediate mode" means there are no widget objects and no retained tree.
 * Every frame you simply ask "draw a button here, was it clicked?" and act on
 * the answer. The only state that has to survive between frames is which
 * control the mouse has captured while dragging -- that lives in ui.c.
 */

/* Shared palette so the menu and the HUD agree on colours. */
#define UI_BG      CLITERAL(Color){  32,  36,  44, 242 }
#define UI_PANEL   CLITERAL(Color){  46,  52,  64, 255 }
#define UI_BORDER  CLITERAL(Color){  76,  86, 106, 255 }
#define UI_TEXT    CLITERAL(Color){ 229, 233, 240, 255 }
#define UI_MUTED   CLITERAL(Color){ 143, 152, 170, 255 }
#define UI_ACCENT  CLITERAL(Color){ 136, 192, 208, 255 }
#define UI_DANGER  CLITERAL(Color){ 191,  97, 106, 255 }

void ui_begin(void);

/* Returns true on the frame the button is released with the cursor inside.
 * A disabled button still draws (greyed) but never reports a click. */
bool ui_button(Rectangle r, const char *label, bool danger, bool enabled);

/* Slider over a discrete index in [0, count-1]. Returns true when it changes. */
bool ui_slider_index(Rectangle r, int *index, int count, bool enabled);

/* "< label >" stepper. Returns true when the index changes. */
bool ui_cycler(Rectangle r, int *index, int count,
               const char *const *labels, bool enabled);

void ui_text(int x, int y, int size, const char *s, Color c);
void ui_text_center(Rectangle r, int size, const char *s, Color c);

#endif /* UI_H */
