#include "model.h"

#include "anim_gltf.h"
#include "raymath.h"

#include <math.h>
#include <string.h>

#ifdef _MSC_VER
#include <excpt.h>
#endif

#define MODEL_DIR     "assets"
#define MODEL_MAX     16        /* how many skins the menu will list          */
#define PATH_MAX_     260
#define NAME_MAX_      64

/* Every imported skin is scaled to FIGHTER_HEIGHT. That handles the practical
 * problem -- one pack authors in centimetres, the next in metres -- but it is
 * also the fairness rule: a bigger model cannot buy a bigger fighter, because
 * the height a fighter is comes from the simulation, not from the file. */

/* Every glTF clip is resampled to a fixed rate on load -- by anim_gltf.c, at
 * the same rate raylib's own importer uses -- so a frame index is this many
 * per second regardless of what the file was authored at. */
#define GLTF_FPS      ANIM_GLTF_FPS

#define BLEND_TIME     0.12f    /* seconds to cross from one clip to the next */

/* Movement clips play at their AUTHORED rate when the fighter moves at walking
 * top speed, and scale with speed from there. This is still distance-driven --
 * frames advanced stay proportional to ground covered -- but the stride is the
 * clip's own (one loop covers CLIP_PACE_SPEED * duration of ground) instead of
 * the box fighter's, which is what the sim's `gait` measures. Pacing an
 * imported walk by `gait` played a 1s Dota cycle in a third of a second. */
#define CLIP_PACE_SPEED   MOVE_MAX

/* Where in an attack clip the strike lands, as a fraction of its length.
 * Character packs put the contact around a third of the way in, with the long
 * recovery tail after it; glTF has no event track to read the true moment
 * from, so this is the convention, not a measurement. */
#define CLIP_STRIKE_POINT 0.35f

/* The simulation's facing of 0 means +Z, and characters generally arrive from
 * glTF facing +Z as well, so no correction is needed for most packs. Set this
 * to 180 if an imported skin runs backwards -- it is per-model, not universal,
 * and it is the first thing to try when a new one looks wrong. */
/* How much of his height a crouching imported skin folds away. Matched to the
 * built-in fighter, which shortens its legs by CROUCH_DROP of LEG_LEN -- about
 * 18% of FIGHTER_HEIGHT -- so the two crouch to the same silhouette height. */
#define CROUCH_SQUASH  0.18f

#ifndef MODEL_YAW
#define MODEL_YAW      0.0f
#endif

#define RAD2DEG_     (180.0f / 3.14159265358979323846f)

/* ------------------------------- the list -------------------------------- */

static char g_file[MODEL_MAX][NAME_MAX_];
static char g_label[MODEL_MAX][NAME_MAX_];
static int  g_count;

/* ------------------------------ the loaded skin -------------------------- */

static char            g_current[NAME_MAX_];   /* "" = the built-in model */
static char            g_base[NAME_MAX_];      /* file name without extension */
static Model           g_model;
static bool            g_loaded;
static ModelAnimation *g_anims;
static int             g_anim_count;
static bool            g_animated;
static bool            g_anims_custom;   /* loaded by anim_gltf.c: free with
                                          * its unload, and the bind pose has
                                          * been rewritten to inverse-bind
                                          * space (see anim_gltf.h) */

static float g_fit;        /* scale that brings the model to MODEL_HEIGHT */
static float g_lift;       /* how far to raise it so the feet reach y = 0 */

static int   g_clip[ANIM_COUNT];   /* which clip plays for each state */
static int   g_swing_clip[ATTACK_CHAIN];   /* per-swing attack clips, and one */
static int   g_air_clip;                   /* for the airborne dive           */
static int   g_matched;            /* states that found a clip of their own */
static int   g_cur = -1, g_prev = -1;
static float g_frame, g_prev_frame, g_blend, g_clock;
static float g_move_frame;   /* walk/sprint playhead, advanced by ground speed */

/* ---------------------------------------------------------------------------
 * Clip naming.
 *
 * Every pack names its clips differently, so each state lists the substrings it
 * will accept, best first, matched case-insensitively. Conventions in the wild:
 *
 *   raylib     "Robot_Idle", "Robot_Walking", "Robot_Punch"
 *   KayKit     "Idle_A", "Walking_A", "Jump_Start", "Jump_Land"
 *   Blender    "CharacterArmature|Idle"
 *   Source 2   "idle", "run", "attack1", "cast1", "death"  (Dota heroes)
 *   Mixamo     "mixamo.com" -- one clip, no useful name at all
 *
 * Two consequences worth spelling out. Source 2 characters have no WALK: heroes
 * only ever run, so walking must be willing to fall back to the run clip or a
 * Dota hero would stand in his idle pose while crossing the arena. And nothing
 * outside a platformer pack has jump clips, so those fall through to idle --
 * a static airborne pose, but an honest one.
 *
 * A state that matches nothing falls back to idle, and idle to the first clip in
 * the file, so even a Mixamo export with one meaningless name still moves.
 * ------------------------------------------------------------------------- */

#define ALIAS_MAX 7

static const char *const CLIP_ALIASES[ANIM_COUNT][ALIAS_MAX] = {
    [ANIM_IDLE]         = { "idle", "stand", "breath", "loadout" },
    [ANIM_WALK]         = { "walk", "jog", "move", "run" },
    /* "haste" outranks "run" here on purpose. Dota has no sprint, but most
     * heroes carry a faster run authored for the Haste rune, which is exactly
     * the pose a sprint wants -- and it is the one clip in the file the plain
     * run should lose to. It is demoted in AVOID, which still holds: that only
     * breaks ties WITHIN an alias, so it never beats a real run for ANIM_WALK. */
    [ANIM_SPRINT]       = { "sprint", "haste", "run", "jog", "walk" },
    [ANIM_CROUCH]       = { "crouch", "duck", "sneak", "idle" },
    /* "flail" comes last on every airborne row, so a pack with real jump clips
     * still wins. It is there for the ones that do not: Dota authors no jump,
     * but ACT_DOTA_FLAIL is what a hero plays while tossed through the air,
     * which is the same problem seen from the other side. Beware the near-miss
     * -- ACT_DOTA_FLAIL_STATUE is a petrified topple and several heroes name
     * it plainly `flail` while the airborne one is `flail_anim`. */
    [ANIM_JUMP_TAKEOFF] = { "jump_start", "jumpstart", "takeoff", "jump", "flail" },
    [ANIM_JUMP_RISE]    = { "jump_up", "jumpup", "rise", "jump", "flail" },
    [ANIM_JUMP_APEX]    = { "jump_idle", "float", "air", "jump", "flail" },
    [ANIM_JUMP_FALL]    = { "fall", "jump_down", "jumpdown", "jump", "flail" },
    [ANIM_LAND]         = { "land", "jump_end", "jumpend", "idle" },
    [ANIM_ATTACK]       = { "attack", "punch", "swing", "slash", "kick", "chop" },
    /* "die" trails "death" because it is the looser of the two -- it is also a
     * substring of words like "soldier" -- but without it a Source 2 hero has
     * no hurt clip at all, since Valve names them dieBack / dieForward. */
    [ANIM_HURT]         = { "hit", "hurt", "damage", "impact", "flinch", "death", "die" },
    [ANIM_SPECIAL]      = { "special", "cast", "ability", "spell", "channel" },
};

/*
 * The attack chain gets a second lookup on top of ANIM_ATTACK: character packs
 * usually ship one clip per swing -- Source 2 numbers them attack01/attack02,
 * others name them -- and playing the same swing three times wastes that. Each
 * row falls through to the generic attack words, and a swing that still finds
 * nothing plays whatever ANIM_ATTACK resolved to, so a one-clip pack behaves
 * exactly as before. Row order is chain order: jab, cross, finisher.
 */
static const char *const SWING_ALIASES[ATTACK_CHAIN][ALIAS_MAX] = {
    { "attack01", "attack1", "jab", "punch", "attack" },
    { "attack02", "attack2", "cross", "swing", "attack" },
    { "attack03", "attack3", "finisher", "heavy", "attack" },
};

/* The airborne swing is one committed dive, not three swings -- see §7 of the
 * architecture notes. Nothing in a Dota export is authored for it, so after
 * the air-specific names it settles for the generic attack. */
static const char *const AIR_SWING_ALIASES[ALIAS_MAX] = {
    "air_attack", "attack_air", "jump_attack", "jumpattack", "attack",
};

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Case-insensitive substring test. No locale, no allocation, no <ctype.h>
 * surprises with signed chars. */
static bool contains(const char *haystack, const char *needle)
{
    for (int i = 0; haystack[i]; i++) {
        int j = 0;
        while (needle[j] && lower(haystack[i + j]) == lower(needle[j])) j++;
        if (!needle[j]) return true;
    }
    return false;
}

/*
 * Substrings that mark a clip as the wrong one to reach for by default. Portrait
 * and loadout poses are menu furniture; victory, defeat and spawn play once at
 * moments we do not have; and the qualified variant of a move (a haste run, an
 * alternate idle) is a worse default than the plain one.
 *
 * These only ever break TIES between clips that already matched the same alias,
 * never exclude -- if a hero's only run is the haste one, the haste one is what
 * he runs with.
 */
static const char *const AVOID[] = {
    "portrait", "loadout", "victory", "defeat", "taunt", "spawn",
    "haste", "_alt", "_old", "generic", "courier",
    /* Cosmetic tiers. A hero export carries a decade of item animations named
     * after him, and an Immortal's run cycle is still not his run cycle. */
    "immortal", "arcana", "prestige", "persona",
};

#define AVOID_COUNT ((int)(sizeof(AVOID) / sizeof(AVOID[0])))

/*
 * Lower is better.
 *
 * Two signals do the real work.
 *
 * The character's OWN name. A Dota hero export carries animations for every
 * cosmetic ever made for him, and those have short unrelated names -- "hh_idle",
 * "ti10_ti7_immortal_run" -- so picking by name length alone lands on an
 * Immortal's run cycle instead of the hero's.
 *
 * And length, which separates a plain move from a qualified one: between
 * pudge_run_anim and pudge_run_haste_anim, the shorter is the one you want.
 */
static int clip_score(const char *name)
{
    int score = 0;
    while (name[score]) score++;

    if (g_base[0] && contains(name, g_base)) score -= 1000;

    for (int i = 0; i < AVOID_COUNT; i++)
        if (contains(name, AVOID[i])) score += 100;

    return score;
}

/*
 * Aliases are tried best-first, and within one alias the best-scoring clip wins
 * rather than the first one encountered. That distinction does nothing on a
 * pack with a dozen clips and everything on a Dota hero with three hundred,
 * where "the first name containing idle" is the portrait pose.
 */
static int find_clip(const char *const aliases[ALIAS_MAX])
{
    for (int a = 0; a < ALIAS_MAX && aliases[a]; a++) {
        int best = -1, best_score = 0;

        for (int i = 0; i < g_anim_count; i++) {
            if (!contains(g_anims[i].name, aliases[a])) continue;

            int score = clip_score(g_anims[i].name);
            if (best < 0 || score < best_score) {
                best = i;
                best_score = score;
            }
        }

        if (best >= 0) return best;
    }
    return -1;
}

static void resolve_clips(void)
{
    g_matched = 0;
    for (int s = 0; s < ANIM_COUNT; s++) {
        g_clip[s] = find_clip(CLIP_ALIASES[s]);
        if (g_clip[s] >= 0) g_matched++;
    }

    int fallback = (g_clip[ANIM_IDLE] >= 0) ? g_clip[ANIM_IDLE]
                                            : (g_anim_count > 0 ? 0 : -1);
    for (int s = 0; s < ANIM_COUNT; s++)
        if (g_clip[s] < 0) g_clip[s] = fallback;

    /* Resolved after the fallbacks so an unmatched swing lands on whatever
     * ANIM_ATTACK plays, never on raw idle. */
    for (int i = 0; i < ATTACK_CHAIN; i++) {
        g_swing_clip[i] = find_clip(SWING_ALIASES[i]);
        if (g_swing_clip[i] < 0) g_swing_clip[i] = g_clip[ANIM_ATTACK];
    }
    g_air_clip = find_clip(AIR_SWING_ALIASES);
    if (g_air_clip < 0) g_air_clip = g_clip[ANIM_ATTACK];
}

int model_clip_count(void)    { return g_anim_count; }
int model_matched_count(void) { return g_matched; }

const char *model_state_clip(AnimState state)
{
    if (!g_animated || state < 0 || state >= ANIM_COUNT) return "";
    int c = g_clip[state];
    return (c >= 0 && c < g_anim_count) ? g_anims[c].name : "";
}

const char *model_swing_clip(int swing, bool air)
{
    if (!g_animated || swing < 0 || swing >= ATTACK_CHAIN) return "";
    int c = air ? g_air_clip : g_swing_clip[swing];
    return (c >= 0 && c < g_anim_count) ? g_anims[c].name : "";
}

/* -------------------------------- scanning ------------------------------- */

static void copy_str(char *dst, const char *src, int max)
{
    int i = 0;
    for (; src[i] && i < max - 1; i++) dst[i] = src[i];
    dst[i] = '\0';
}

void model_scan(void)
{
    g_count = 1;
    copy_str(g_file[0], "", NAME_MAX_);
    copy_str(g_label[0], "Built-in blocks", NAME_MAX_);

    const char *dir = TextFormat("%s%s", GetApplicationDirectory(), MODEL_DIR);
    if (!DirectoryExists(dir)) return;

    FilePathList list = LoadDirectoryFilesEx(dir, ".glb;.gltf", false);
    for (unsigned int i = 0; i < list.count && g_count < MODEL_MAX; i++) {
        copy_str(g_file[g_count], GetFileName(list.paths[i]), NAME_MAX_);
        copy_str(g_label[g_count], GetFileNameWithoutExt(list.paths[i]), NAME_MAX_);
        g_count++;
    }
    UnloadDirectoryFiles(list);
}

int model_count(void) { return g_count > 0 ? g_count : 1; }

const char *model_file(int index)
{
    return (index > 0 && index < g_count) ? g_file[index] : "";
}

const char *model_label(int index)
{
    return (index >= 0 && index < g_count) ? g_label[index] : "Built-in blocks";
}

int model_index_of(const char *file)
{
    if (!file || !file[0]) return 0;
    for (int i = 0; i < g_count; i++)
        if (strncmp(g_file[i], file, NAME_MAX_) == 0) return i;
    return -1;
}

/* ------------------------------ load / unload ---------------------------- */

void model_unload(void)
{
    if (g_anims) {
        if (g_anims_custom) anim_gltf_unload(g_anims, g_anim_count);
        else UnloadModelAnimations(g_anims, g_anim_count);
        g_anims = NULL;
    }
    if (g_loaded) UnloadModel(g_model);

    g_anim_count = 0;
    g_matched = 0;
    g_loaded = false;
    g_animated = false;
    g_anims_custom = false;
    g_cur = g_prev = -1;
    g_blend = 0.0f;
    g_move_frame = 0.0f;
    g_current[0] = '\0';
    g_base[0] = '\0';
}

/*
 * A model file is untrusted input, exactly like the save file: it was written by
 * a converter we did not write, from a file we did not make. raylib's glTF
 * parsers will walk off the end of a malformed one and take the process with
 * them -- below anything this module can inspect beforehand, so `FileExists` and
 * a mesh-count check are not enough.
 *
 * On MSVC we can at least catch it and carry on. A skin that fails this way
 * loses its animations, or is skipped entirely, rather than ending the game --
 * which matters for a feature whose whole point is loading other people's
 * exports.
 */
#ifdef _MSC_VER
#define GUARDED(expr, on_fail)                          \
    __try { expr; }                                     \
    __except (EXCEPTION_EXECUTE_HANDLER) { on_fail; }
#else
#define GUARDED(expr, on_fail) expr;
#endif

static bool load_model_guarded(const char *path, Model *out)
{
    bool ok = true;
    GUARDED(*out = LoadModel(path), ok = false)
    return ok;
}

/*
 * Animations come from our own importer (anim_gltf.c), which exists because
 * raylib's mis-skins any rig whose inverse binds live in a different unit
 * space than its nodes -- every Source 2 export does. When ours declines the
 * file, raylib's is still tried: for the rigs it handles (Mixamo, KayKit,
 * its own robot) an import through either produces the same poses, and a
 * skin that animates imperfectly beats one that stands still.
 */
static ModelAnimation *load_anims_guarded(const char *path, int *count,
                                          Matrix *rest_transform)
{
    ModelAnimation *anims = NULL;
    *count = 0;
    *rest_transform = MatrixIdentity();

    GUARDED(anims = anim_gltf_load(path, &g_model.skeleton, count, rest_transform),
            (*count = 0, *rest_transform = MatrixIdentity(), anims = NULL))
    g_anims_custom = (anims != NULL);

    if (!anims)
        GUARDED(anims = LoadModelAnimations(path, count), (*count = 0, anims = NULL))
    return anims;
}

/*
 * The box the model occupies while standing in its idle pose.
 *
 * The fit below wants to know how tall the character is and where his feet
 * are, and the obvious source -- the raw mesh vertices -- answers a subtly
 * different question: it measures the BIND pose. On a body-only export the two
 * agree closely enough. On a full export they do not: a hero's bind pose holds
 * his weapon hanging point-down past his heels, so the raw box's floor is the
 * tip of a club. Fitting to that shrinks the hero to make room for it and then
 * lifts him until the club touches the ground and he does not -- measured on
 * Ogre Magi with his gear, 8.7% short and floating 0.16 units.
 *
 * So pose him first. UpdateModelAnimation runs the same CPU skinning the
 * renderer will, leaving the result in mesh.animVertices, which is exactly the
 * space the player sees -- unit conversion, axis conversion and all.
 *
 * That leaves the other end of the character. A full export also hands him the
 * things he CARRIES, and Witch Doctor's staff stands a third again as tall as
 * he does -- fit the whole silhouette and he is scaled down to make room for
 * his own staff, ending up 29% shorter than everyone else. What separates a
 * staff from a hat is not size, position or width (all three were measured,
 * none of them separate): it is that a body DEFORMS as it moves and a carried
 * prop does not. So the height is measured over the meshes that bend, and a
 * mesh is judged by whether the distance between two of its vertices survives
 * a change of pose.
 *
 * The fallback matters as much as the rule: raylib's robot is rigid parts all
 * the way down, and dropping every rigid mesh would leave nothing to measure.
 * When the deforming meshes are not most of the model, nothing is a prop.
 *
 * Returns false when there is nothing to pose with (no clips, or a mesh with no
 * skin weights), and the caller falls back to the raw box.
 */
#define RIGID_SAMPLES  48     /* vertex pairs measured per mesh          */
#define RIGID_TOL      0.01f  /* 1% -- float noise, not a bending limb   */
#define RIGID_MESH_MAX 32     /* beyond this a mesh is assumed to deform */

static void mesh_pair_spans(const Mesh *mesh, float *span)
{
    /* Any two points of a rigid body hold their distance however it is placed,
     * so real edges are unnecessary -- pairs picked by index do just as well. */
    int n = mesh->vertexCount;
    for (int s = 0; s < RIGID_SAMPLES; s++) {
        int a = (n > 1) ? (int)((long long)s * n / RIGID_SAMPLES) : 0;
        int b = (a + n / 2) % (n > 0 ? n : 1);
        const float *pa = &mesh->animVertices[a * 3];
        const float *pb = &mesh->animVertices[b * 3];
        float dx = pa[0] - pb[0], dy = pa[1] - pb[1], dz = pa[2] - pb[2];
        span[s] = sqrtf(dx * dx + dy * dy + dz * dz);
    }
}

static bool pose_bounding_box(BoundingBox *out)
{
    if (!g_animated || g_clip[ANIM_IDLE] < 0) return false;

    ModelAnimation anim = g_anims[g_clip[ANIM_IDLE]];
    int count = g_model.meshCount;
    if (count > RIGID_MESH_MAX) count = RIGID_MESH_MAX;

    static float span_a[RIGID_MESH_MAX][RIGID_SAMPLES];
    static float span_b[RIGID_MESH_MAX][RIGID_SAMPLES];
    Vector3 mesh_lo[RIGID_MESH_MAX], mesh_hi[RIGID_MESH_MAX];
    bool deforms[RIGID_MESH_MAX];

    /* First pose: the boxes come from here, so every later measurement is of
     * the same stance. */
    UpdateModelAnimation(g_model, anim, 0.0f);

    int total = 0;
    for (int m = 0; m < count; m++) {
        const Mesh *mesh = &g_model.meshes[m];
        deforms[m] = true;
        mesh_lo[m] = (Vector3){  1e9f,  1e9f,  1e9f };
        mesh_hi[m] = (Vector3){ -1e9f, -1e9f, -1e9f };
        if (!mesh->animVertices || mesh->vertexCount <= 0) continue;
        for (int v = 0; v < mesh->vertexCount; v++) {
            Vector3 p = { mesh->animVertices[v * 3 + 0],
                          mesh->animVertices[v * 3 + 1],
                          mesh->animVertices[v * 3 + 2] };
            mesh_lo[m] = Vector3Min(mesh_lo[m], p);
            mesh_hi[m] = Vector3Max(mesh_hi[m], p);
        }
        mesh_pair_spans(mesh, span_a[m]);
        total += mesh->vertexCount;
    }
    if (total == 0) return false;

    /* Second pose, far enough along the clip to have moved. A one-frame clip
     * cannot answer the question, and then nothing is a prop. */
    int soft = 0;
    if (anim.keyframeCount > 1) {
        UpdateModelAnimation(g_model, anim, (float)(anim.keyframeCount / 2));
        for (int m = 0; m < count; m++) {
            const Mesh *mesh = &g_model.meshes[m];
            if (!mesh->animVertices || mesh->vertexCount <= 0) continue;
            mesh_pair_spans(mesh, span_b[m]);
            bool rigid = true;
            for (int s = 0; s < RIGID_SAMPLES; s++) {
                float a = span_a[m][s];
                if (a <= 1e-4f) continue;
                if (fabsf(span_b[m][s] - a) / a > RIGID_TOL) { rigid = false; break; }
            }
            deforms[m] = !rigid;
            if (deforms[m]) soft += mesh->vertexCount;
        }
        UpdateModelAnimation(g_model, anim, 0.0f);   /* leave him standing */
    }

    bool props = (anim.keyframeCount > 1) && (soft * 2 >= total);

    Vector3 lo = {  1e9f,  1e9f,  1e9f };
    Vector3 hi = { -1e9f, -1e9f, -1e9f };
    for (int m = 0; m < count; m++) {
        if (mesh_hi[m].x < mesh_lo[m].x) continue;      /* nothing measured */
        if (props && !deforms[m]) continue;
        lo = Vector3Min(lo, mesh_lo[m]);
        hi = Vector3Max(hi, mesh_hi[m]);
    }

    if (hi.x < lo.x) return false;
    out->min = lo;
    out->max = hi;
    return true;
}

static bool load(const char *file)
{
    const char *path = TextFormat("%s%s/%s", GetApplicationDirectory(),
                                  MODEL_DIR, file);
    if (!FileExists(path)) return false;

    /* "pudge.glb" -> "pudge", which is what its own animations are named after. */
    copy_str(g_base, GetFileNameWithoutExt(path), NAME_MAX_);

    if (!load_model_guarded(path, &g_model)) return false;
    if (g_model.meshCount == 0) {       /* unreadable, or not a model at all */
        UnloadModel(g_model);
        return false;
    }
    g_loaded = true;

    /* Animations before the fit: their importer reports the rest conversion
     * the skinning applies to the raw vertices, and the fit and up-axis test
     * below must look through it.
     *
     * A model with no skeleton is still perfectly drawable -- it just stands
     * there. Worth allowing: it makes a static prop or a sculpt usable as a
     * skin without pretending it can animate. */
    Matrix rest_c = MatrixIdentity();
    if (g_model.skeleton.boneCount > 0) {
        g_anims = load_anims_guarded(path, &g_anim_count, &rest_c);
        if (!g_anims) g_anim_count = 0;

        /* Drop any clip built for a different skeleton: handing one to
         * UpdateModelAnimation reads bones that are not there. */
        int keep = 0;
        for (int i = 0; i < g_anim_count; i++)
            if (IsModelAnimationValid(g_model, g_anims[i])) g_anims[keep++] = g_anims[i];
        g_anim_count = keep;

        g_animated = (g_anim_count > 0);
        if (g_animated) resolve_clips();
    }

    /*
     * Which way is up?
     *
     * glTF says +Y, but Source 2 exports -- every Dota hero -- come out +Z, and
     * an uncorrected one lies on its face. The reliable tell is not which axis
     * is longest (a robot with outstretched arms is wider than it is tall, and
     * Pudge's hook chain stretches his Y almost as far as his height): it is
     * WHERE THE FEET ARE. A character is authored standing on its ground plane,
     * so the up axis is the one whose minimum sits at roughly zero, while the
     * other runs well negative.
     */
    BoundingBox bb;
    bool posed = pose_bounding_box(&bb);

    /* That box measures the raw vertices -- but an animated mesh renders
     * through the skinning matrices, whose rest pose applies whatever
     * unit-and-axis conversion the file's hierarchy carries (inches to metres
     * AND Z-up to Y-up, on a Source 2 rig). Judge the box the skinning will
     * actually produce: testing the raw one means inspecting a space the
     * player never sees, and the "correction" below then lays an
     * already-upright hero on his face.
     *
     * A posed box already went through the skinning and so needs none of this. */
    if (!posed && g_animated && g_anims_custom) {
        Vector3 lo = {  1e9f,  1e9f,  1e9f };
        Vector3 hi = { -1e9f, -1e9f, -1e9f };
        for (int i = 0; i < 8; i++) {
            Vector3 corner = {
                (i & 1) ? bb.max.x : bb.min.x,
                (i & 2) ? bb.max.y : bb.min.y,
                (i & 4) ? bb.max.z : bb.min.z,
            };
            corner = Vector3Transform(corner, rest_c);
            lo = Vector3Min(lo, corner);
            hi = Vector3Max(hi, corner);
        }
        bb.min = lo;
        bb.max = hi;
    }

    float ext_y = bb.max.y - bb.min.y;
    float ext_z = bb.max.z - bb.min.z;

    float floor_y = (ext_y > 0.0001f) ? fabsf(bb.min.y) / ext_y : 1.0f;
    float floor_z = (ext_z > 0.0001f) ? fabsf(bb.min.z) / ext_z : 1.0f;
    bool z_up = (floor_z < floor_y);

    float height = z_up ? ext_z : ext_y;
    float floor  = z_up ? bb.min.z : bb.min.y;

    g_fit  = (height > 0.0001f) ? (FIGHTER_HEIGHT / height) : 1.0f;
    g_lift = -floor * g_fit;

    /* Applied before the draw-time position/rotation/scale, so it corrects the
     * mesh AND everything the skeleton does to it. */
    g_model.transform = z_up ? MatrixRotateX(-PI * 0.5f) : MatrixIdentity();

    g_cur = g_prev = -1;
    g_blend = 0.0f;
    g_move_frame = 0.0f;
    return true;
}

bool model_active(const char *file)
{
    if (!file) file = "";
    if (strncmp(file, g_current, NAME_MAX_) == 0) return g_loaded;

    model_unload();
    copy_str(g_current, file, NAME_MAX_);

    if (!file[0]) return false;             /* the built-in was asked for */
    if (!load(file)) {
        /* Missing or broken: fall back rather than refusing to draw anything.
         * The name is remembered so we do not retry the same failure every
         * frame, and so the menu still shows what was selected. */
        model_unload();
        copy_str(g_current, file, NAME_MAX_);
        return false;
    }
    return true;
}

bool model_is_mesh(void) { return g_loaded; }

/* ------------------------------- playback -------------------------------- */

static float clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/*
 * Where the playhead should be, in frames. The simulation decides -- this only
 * translates its state into a position in a clip.
 */
static float state_frame(const Fighter *f, AnimState state, const ModelAnimation *a)
{
    float last = (a->keyframeCount > 1) ? (float)(a->keyframeCount - 1) : 0.0f;
    if (last <= 0.0f) return 0.0f;

    switch (state) {
    case ANIM_WALK:
    case ANIM_SPRINT:
        /* Advanced in model_animate by ground speed against CLIP_PACE_SPEED:
         * the clip runs at its authored rate at walking speed, faster in a
         * sprint, slower easing out of a stop -- and the feet still track the
         * ground, because frames advanced stay proportional to distance. */
        return fminf(g_move_frame, last);

    case ANIM_ATTACK: {
        /* At the clip's AUTHORED rate, anchored so its strike moment lands on
         * the simulation's impact. Compressing the whole clip into the swing
         * -- the old scheme -- played a 1.3s Dota attack at 4x; at 1x only the
         * window around the strike fits in the swing, so the long wind-up is
         * skipped and the blend out eats the recovery tail. */
        float strike = CLIP_STRIKE_POINT * last;
        float frame  = strike
                     + (f->attack_time - fighter_attack_impact_time(f)) * GLTF_FPS;
        return (frame < 0.0f) ? 0.0f : fminf(frame, last);
    }

    case ANIM_JUMP_TAKEOFF:
    case ANIM_JUMP_RISE:
    case ANIM_JUMP_APEX:
    case ANIM_JUMP_FALL:
        /* One-shot from the moment the feet left the ground. Packs usually
         * have a single jump clip covering the whole arc, so all four states
         * play through it rather than restarting it four times. */
        return fminf(f->air_time * GLTF_FPS, last);

    case ANIM_LAND:
        return fminf(f->anim_time * GLTF_FPS, last);

    default: {
        /* Idle and the rest simply loop on the clock. */
        float frame = fmodf(g_clock * GLTF_FPS, last);
        return (frame < 0.0f) ? 0.0f : frame;
    }
    }
}

void model_animate(const Fighter *f, float dt)
{
    if (!g_loaded || !g_animated) return;

    g_clock += dt;

    AnimState state = (f->anim >= 0 && f->anim < ANIM_COUNT) ? f->anim : ANIM_IDLE;
    int want = g_clip[state];

    /* Attacking narrows the choice to the specific swing: the chain index
     * picks among the pack's attack variants, and the airborne dive gets its
     * own clip. attack_air is latched by the simulation, so the clip cannot
     * change under a swing when the ground arrives mid-way through it. */
    if (state == ANIM_ATTACK) {
        int i = (f->attack_index >= 0 && f->attack_index < ATTACK_CHAIN)
                    ? f->attack_index : 0;
        int swing = f->attack_air ? g_air_clip : g_swing_clip[i];
        if (swing >= 0) want = swing;
    }

    if (want < 0) return;

    /* The walk/sprint playhead, in frames of the clip about to play. Advance
     * is proportional to ground speed -- authored rate at CLIP_PACE_SPEED --
     * and wraps against this clip's length, so the cycle loops seamlessly. */
    if (state == ANIM_WALK || state == ANIM_SPRINT) {
        float last = (g_anims[want].keyframeCount > 1)
                   ? (float)(g_anims[want].keyframeCount - 1) : 0.0f;
        if (last > 0.0f) {
            float speed = sqrtf(f->vx * f->vx + f->vz * f->vz);
            g_move_frame = fmodf(g_move_frame
                                 + speed / CLIP_PACE_SPEED * GLTF_FPS * dt, last);
        }
    }

    if (g_cur < 0) {
        g_cur = want;
        g_blend = 1.0f;
    } else if (want != g_cur) {
        /* Cross from the pose we were in, frozen where it was, into the new
         * clip. Freezing the outgoing frame is a simplification, but over a
         * tenth of a second it is indistinguishable from playing both. */
        g_prev = g_cur;
        g_prev_frame = g_frame;
        g_cur = want;
        g_blend = 0.0f;
    } else if (g_blend < 1.0f) {
        g_blend = clamp01(g_blend + dt / BLEND_TIME);
    }

    g_frame = state_frame(f, state, &g_anims[g_cur]);

    if (g_prev < 0 || g_blend >= 1.0f)
        UpdateModelAnimation(g_model, g_anims[g_cur], g_frame);
    else
        UpdateModelAnimationEx(g_model, g_anims[g_prev], g_prev_frame,
                               g_anims[g_cur], g_frame, g_blend);
}

bool model_draw(const Fighter *f, Color tint)
{
    if (!g_loaded) return false;

    /*
     * Crouch, for a skeleton nobody authored one for.
     *
     * No character pack outside a platformer ships a crouch, and Dota
     * certainly does not, so ANIM_CROUCH falls back to idle and an imported
     * fighter used to duck by standing perfectly still. Rather than pose the
     * skeleton -- which would need to know where its hips are, and every rig
     * names them differently -- squash the whole model vertically about its
     * feet. That is the same trick the built-in fighter uses (§8: there are no
     * knees in a one-box limb, so it shortens the legs and drops everything
     * riding on them), and it works here for the same reason: it reads from
     * every camera angle and needs to know nothing about the rig.
     *
     * The landing squash rides along in the same value, which is why an
     * imported skin now compresses on touchdown as the box fighter does.
     */
    float dip    = fminf(1.0f, f->crouch_blend + f->land_blend * 0.85f);
    float squash = 1.0f - CROUCH_SQUASH * dip;

    /* The lift scales with the squash or the feet leave the floor: it is the
     * distance from the model's origin down to its feet, and that distance is
     * being compressed too. */
    DrawModelEx(g_model,
                (Vector3){ f->x, f->y + g_lift * squash, f->z },
                (Vector3){ 0.0f, 1.0f, 0.0f },
                f->facing * RAD2DEG_ + MODEL_YAW,
                (Vector3){ g_fit, g_fit * squash, g_fit },
                tint);
    return true;
}
