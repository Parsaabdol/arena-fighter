#ifndef ANIM_GLTF_H
#define ANIM_GLTF_H

#include "raylib.h"

/*
 * Our own glTF animation importer.
 *
 * raylib skins with boneMatrix = inv(bindPose) * animPose, where BOTH sides
 * come from the file's node hierarchy rather than from its inverse bind
 * matrices. That is self-consistent right up until a file's skinning data
 * lives in a different space than its nodes -- which is exactly what a
 * Source 2 export is: vertices and inverse binds authored in inches, the
 * skeleton hung under a 0.0254 unit-conversion node, so node space is metres.
 * The composed boneMatrix then comes out as the metre-space CONJUGATE of the
 * correct inch-space transform: rotations survive, translations are 39x too
 * small, and every bone pivots around the model origin instead of its joint.
 * Near the rest pose that error is invisible (boneMatrix ~ identity); a run
 * cycle is mangled. This is §12's "Source 2 rigs deform" bug.
 *
 * The fix is to make the runtime's formula reproduce what the glTF spec asks
 * for, skinMatrix = jointWorld(t) * inverseBindMatrix, using only data we are
 * allowed to write: this loader parses the file with cgltf, composes every
 * pose through the FULL node hierarchy (animated ancestors included, in
 * whatever order the exporter wrote the nodes), and rewrites the model's
 * bindPose from the file's inverse bind matrices so inv(bindPose) IS the
 * inverse bind. Playback, blending and skinning stay raylib's.
 */

/* Clips are resampled to this fixed rate, matching what raylib's own importer
 * does, so a frame index means the same thing whichever loader filled it. */
#define ANIM_GLTF_FPS 60.0f

/*
 * Load every animation in a .glb/.gltf as raylib ModelAnimation data, posed
 * for `skeleton` (the skeleton raylib built when the model itself loaded --
 * bone order here mirrors the file's skin, which is how the two stay in
 * agreement). On success the skeleton's bindPose is REWRITTEN into
 * inverse-bind space to match, which is why the pointer is not const; on
 * failure the skeleton is untouched. Returns NULL when the file has no usable
 * skin or animations.
 *
 * `rest_transform` receives the rig-wide conversion the skinning applies to
 * the raw vertices at rest: jointWorld(rest) * inverseBind, constant across a
 * consistent rig's bones (verified across every bone, nearest-to-median bone
 * wins). Identity for most packs; rotation Z-up to Y-up plus scale 0.0254 for
 * an inch-authored Source 2 rig. An animated mesh renders through it, the
 * static bind mesh does not -- so up-axis detection and the height fit must
 * look at the raw bounding box pushed through this, or they will judge (and
 * then "correct") the wrong space, which is how a hero ends up face-down.
 *
 * Free the result with anim_gltf_unload, not UnloadModelAnimations: the
 * allocator is this module's, not raylib's.
 */
ModelAnimation *anim_gltf_load(const char *path, ModelSkeleton *skeleton,
                               int *count, Matrix *rest_transform);
void anim_gltf_unload(ModelAnimation *anims, int count);

#endif /* ANIM_GLTF_H */
