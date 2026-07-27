#ifndef MODEL_H
#define MODEL_H

#include <stdbool.h>

#include "game.h"
#include "raylib.h"

/*
 * Imported character skins.
 *
 * The game ships with no models. This scans a local `assets/` directory beside
 * the executable for glTF files and, if it finds any, draws one instead of the
 * built-in box fighter. Nothing here is required: with an empty or missing
 * `assets/` the game runs exactly as it did before, which is what keeps the
 * repository free of anyone else's art.
 *
 * The simulation still drives the pose. A clip's playhead is not a wall clock:
 * movement clips advance with ground speed -- their authored rate at walking
 * top speed, so a run cycle looks the way its source game played it -- and an
 * attack clip runs at its authored rate anchored so its strike frame lands on
 * the simulation's impact moment. An imported skin keeps the properties the
 * procedural model has -- feet that track the ground, a strike that reads on
 * the hit window -- without being played faster than it was made to go.
 */

/* Scan `assets/` for .glb/.gltf. Call once, after the window exists. */
void model_scan(void);

/* The list always has at least one entry: index 0 is the built-in box model,
 * whose file name is "" and whose label says so. */
int         model_count(void);
const char *model_file(int index);    /* "" for the built-in */
const char *model_label(int index);   /* what the menu shows */
int         model_index_of(const char *file);   /* -1 when not present */

/*
 * Make `file` the active skin, loading it if it is not already loaded. Passing
 * "" -- or a name that is missing, unreadable or has no skeleton -- selects the
 * built-in model. Cheap to call every frame; it only does work on a change.
 * Returns true when a mesh skin is active.
 */
bool model_active(const char *file);

/* Whether a mesh skin is currently drawn, as opposed to the built-in fighter.
 * Read-only: the menu uses it to say whether a chosen file actually loaded. */
bool model_is_mesh(void);

/*
 * How the active skin's animations turned out: how many clips the file has, and
 * how many of the game's states found a clip of their own rather than falling
 * back to idle. The menu reports both, because "it loaded" and "it animates"
 * are different kinds of success and an export can easily manage only the
 * first.
 */
int model_clip_count(void);
int model_matched_count(void);

/* Which clip a state actually resolved to, by name, or "" when the active skin
 * has no animations. The menu reports the totals; this explains them. */
const char *model_state_clip(AnimState state);

/* Likewise for the attack chain: which clip swing 0..ATTACK_CHAIN-1 plays on
 * the ground, or the airborne dive when `air` is set. Falls back to whatever
 * ANIM_ATTACK resolved to, so it is never "" while the skin animates. */
const char *model_swing_clip(int swing, bool air);

/* Advance the active skin's clips. Wall-clock dt: this is presentation. */
void model_animate(const Fighter *f, float dt);

/* Draw the active skin, scaled to FIGHTER_HEIGHT whatever size it was authored
 * at. Returns false when there is none, and the caller draws the built-in model
 * instead. */
bool model_draw(const Fighter *f, Color tint);

void model_unload(void);

#endif /* MODEL_H */
