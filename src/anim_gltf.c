#include "anim_gltf.h"

#include "raymath.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Declarations only -- deliberately NO CGLTF_IMPLEMENTATION. raylib compiles
 * cgltf into raylib.lib with external linkage, so the implementation is
 * already in the link; a second copy here would collide with it. The vendored
 * header is pinned to 1.15, the same version raylib 6.0 bundles, which is
 * what makes sharing the compiled implementation (and its struct layout)
 * sound. If the raylib in vendor/ moves, move vendor/cgltf/ with it. */
#include "cgltf.h"

/* A model file is untrusted input, and a clip's duration is a number the file
 * simply states. Resampling is duration * 60 frames of Transform[boneCount],
 * so an absurd duration is an absurd allocation: cap it at two minutes, well
 * above the longest clip a character pack has any business shipping. */
#define MAX_KEYFRAMES (2 * 60 * (int)ANIM_GLTF_FPS)

/* Per-channel sampling data, unpacked out of the accessors once so the
 * per-frame loop reads plain floats instead of re-decoding buffer views. */
typedef struct Track {
    int    node;        /* index into data->nodes                            */
    int    path;        /* cgltf_animation_path_type                         */
    int    interp;      /* cgltf_interpolation_type                          */
    int    keys;        /* keyframe count                                    */
    int    comps;       /* floats per value: 3 for T/S, 4 for R              */
    float *times;
    float *vals;        /* keys values; cubic spline keeps 3 per key (in-    */
                        /* tangent, value, out-tangent), others keep 1       */
} Track;

/* glTF stores matrices column-major; raylib's struct fields are laid out so
 * this straight-through mapping is the correct one (same as raylib's own). */
static Matrix mat_from_gltf(const cgltf_float *m)
{
    Matrix r = {
        m[0], m[4], m[8],  m[12],
        m[1], m[5], m[9],  m[13],
        m[2], m[6], m[10], m[14],
        m[3], m[7], m[11], m[15],
    };
    return r;
}

/* Largest key with times[k] <= t, clamped so k+1 is always readable. */
static int key_before(const float *times, int keys, float t)
{
    int lo = 0, hi = keys - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (times[mid] <= t) lo = mid;
        else hi = mid - 1;
    }
    return (lo > keys - 2) ? (keys > 1 ? keys - 2 : 0) : lo;
}

/* Where between key k and k+1 the time t lands, 0..1. */
static float key_fraction(const Track *tr, int k, float t, float *span)
{
    *span = 0.0f;
    if (tr->keys < 2) return 0.0f;

    float t0 = tr->times[k], t1 = tr->times[k + 1];
    if (t1 <= t0) return 0.0f;

    *span = t1 - t0;
    float u = (t - t0) / *span;
    return (u < 0.0f) ? 0.0f : (u > 1.0f ? 1.0f : u);
}

/* Value element for key k. Cubic-spline output is (in-tangent, value,
 * out-tangent) per key, so the value sits one element in. */
static const float *track_value(const Track *tr, int k)
{
    int stride = (tr->interp == cgltf_interpolation_type_cubic_spline) ? 3 : 1;
    int offset = (stride == 3) ? 1 : 0;
    return tr->vals + (size_t)(k * stride + offset) * tr->comps;
}

static const float *track_tangent(const Track *tr, int k, bool out)
{
    return tr->vals + (size_t)(k * 3 + (out ? 2 : 0)) * tr->comps;
}

static Vector3 sample_vec3(const Track *tr, float t)
{
    int k = key_before(tr->times, tr->keys, t);
    float span;
    float u = key_fraction(tr, k, t, &span);

    const float *a = track_value(tr, k);
    Vector3 v1 = { a[0], a[1], a[2] };
    if (tr->keys < 2 || span <= 0.0f) return v1;

    const float *b = track_value(tr, k + 1);
    Vector3 v2 = { b[0], b[1], b[2] };

    switch (tr->interp) {
    case cgltf_interpolation_type_step:
        return v1;
    case cgltf_interpolation_type_cubic_spline: {
        /* Spec: tangents are per-second, scaled by the key interval. */
        const float *ta = track_tangent(tr, k, true);
        const float *tb = track_tangent(tr, k + 1, false);
        Vector3 m1 = Vector3Scale((Vector3){ ta[0], ta[1], ta[2] }, span);
        Vector3 m2 = Vector3Scale((Vector3){ tb[0], tb[1], tb[2] }, span);
        return Vector3CubicHermite(v1, m1, v2, m2, u);
    }
    default:
        return Vector3Lerp(v1, v2, u);
    }
}

static Quaternion sample_quat(const Track *tr, float t)
{
    int k = key_before(tr->times, tr->keys, t);
    float span;
    float u = key_fraction(tr, k, t, &span);

    const float *a = track_value(tr, k);
    Quaternion q1 = { a[0], a[1], a[2], a[3] };
    if (tr->keys < 2 || span <= 0.0f) return QuaternionNormalize(q1);

    const float *b = track_value(tr, k + 1);
    Quaternion q2 = { b[0], b[1], b[2], b[3] };

    switch (tr->interp) {
    case cgltf_interpolation_type_step:
        return QuaternionNormalize(q1);
    case cgltf_interpolation_type_cubic_spline: {
        const float *ta = track_tangent(tr, k, true);
        const float *tb = track_tangent(tr, k + 1, false);
        q1 = QuaternionNormalize(q1);
        q2 = QuaternionNormalize(q2);
        /* Keep the pair in one hemisphere or the spline takes the long way. */
        if (q1.x*q2.x + q1.y*q2.y + q1.z*q2.z + q1.w*q2.w < 0.0f)
            q2 = (Quaternion){ -q2.x, -q2.y, -q2.z, -q2.w };
        Quaternion m1 = { ta[0]*span, ta[1]*span, ta[2]*span, ta[3]*span };
        Quaternion m2 = { tb[0]*span, tb[1]*span, tb[2]*span, tb[3]*span };
        return QuaternionNormalize(
            QuaternionCubicHermiteSpline(q1, m1, q2, m2, u));
    }
    default:
        /* QuaternionSlerp already takes the short way round. */
        return QuaternionNormalize(QuaternionSlerp(q1, q2, u));
    }
}

/* ------------------------- per-frame pose building ------------------------ */

typedef struct NodeAnim {
    int t_track, r_track, s_track;    /* indices into tracks, -1 = unanimated */
} NodeAnim;

/*
 * A node's LOCAL transform at time t: the file's rest TRS (or static matrix)
 * with any animated channel sampled over it. This is the piece raylib's
 * importer only does for nodes inside the skin's joint list; doing it for
 * every node is most of what this module exists for.
 */
static Matrix node_local(const cgltf_node *n, const NodeAnim *na,
                         const Track *tracks, float t)
{
    bool animated = na->t_track >= 0 || na->r_track >= 0 || na->s_track >= 0;

    if (n->has_matrix && !animated) return mat_from_gltf(n->matrix);

    Vector3    tr = { n->translation[0], n->translation[1], n->translation[2] };
    Quaternion ro = { n->rotation[0], n->rotation[1], n->rotation[2], n->rotation[3] };
    Vector3    sc = { n->scale[0], n->scale[1], n->scale[2] };

    if (na->t_track >= 0) tr = sample_vec3(&tracks[na->t_track], t);
    if (na->r_track >= 0) ro = sample_quat(&tracks[na->r_track], t);
    if (na->s_track >= 0) sc = sample_vec3(&tracks[na->s_track], t);

    return MatrixMultiply(MatrixMultiply(MatrixScale(sc.x, sc.y, sc.z),
                                         QuaternionToMatrix(ro)),
                          MatrixTranslate(tr.x, tr.y, tr.z));
}

/* Model-space transform of a node, composed up the REAL parent chain with
 * memoisation per frame. Recursion depth is the rig's hierarchy depth. */
static Matrix node_global(const cgltf_data *d, const cgltf_node *n,
                          const Matrix *locals, Matrix *cache, bool *have)
{
    size_t i = (size_t)(n - d->nodes);
    if (!have[i]) {
        have[i] = true;    /* set first: a (malformed) parent cycle then
                            * yields a stale identity instead of recursing
                            * forever on untrusted input */
        cache[i] = MatrixIdentity();
        Matrix g = locals[i];
        if (n->parent)
            g = MatrixMultiply(g, node_global(d, n->parent, locals, cache, have));
        cache[i] = g;
    }
    return cache[i];
}

/* --------------------------------- loading ------------------------------- */

static void free_tracks(Track *tracks, int count)
{
    for (int i = 0; i < count; i++) {
        if (tracks[i].times) MemFree(tracks[i].times);
        if (tracks[i].vals) MemFree(tracks[i].vals);
    }
    if (tracks) MemFree(tracks);
}

void anim_gltf_unload(ModelAnimation *anims, int count)
{
    if (!anims) return;
    for (int i = 0; i < count; i++) {
        if (!anims[i].keyframePoses) continue;
        for (int f = 0; f < anims[i].keyframeCount; f++)
            if (anims[i].keyframePoses[f]) MemFree(anims[i].keyframePoses[f]);
        MemFree(anims[i].keyframePoses);
    }
    MemFree(anims);
}

/*
 * Unpack one animation channel into a Track. Returns false (and adds nothing)
 * for channels this loader cannot or should not use -- a morph-target channel,
 * a count mismatch between input and output, an accessor that fails to unpack.
 * Skipping a channel degrades one property of one node; failing the file over
 * it would throw away the clip.
 */
static bool track_build(const cgltf_data *d, const cgltf_animation_channel *ch,
                        Track *out)
{
    if (!ch->target_node || !ch->sampler) return false;
    if (!ch->sampler->input || !ch->sampler->output) return false;

    size_t node = (size_t)(ch->target_node - d->nodes);
    if (node >= d->nodes_count) return false;

    int comps;
    if (ch->target_path == cgltf_animation_path_type_rotation) comps = 4;
    else if (ch->target_path == cgltf_animation_path_type_translation ||
             ch->target_path == cgltf_animation_path_type_scale) comps = 3;
    else return false;    /* weights (morph targets) are not skeletal */

    if ((int)cgltf_num_components(ch->sampler->output->type) != comps) return false;

    size_t keys = ch->sampler->input->count;
    int stride = (ch->sampler->interpolation == cgltf_interpolation_type_cubic_spline) ? 3 : 1;
    if (keys < 1 || keys > (size_t)1 << 20) return false;
    if (ch->sampler->output->count != keys * (size_t)stride) return false;

    float *times = MemAlloc((unsigned int)(keys * sizeof(float)));
    float *vals  = MemAlloc((unsigned int)(keys * (size_t)stride * (size_t)comps * sizeof(float)));
    if (!times || !vals ||
        cgltf_accessor_unpack_floats(ch->sampler->input, times, keys) < keys ||
        cgltf_accessor_unpack_floats(ch->sampler->output, vals,
                                     keys * (size_t)stride * (size_t)comps)
            < keys * (size_t)stride * (size_t)comps) {
        if (times) MemFree(times);
        if (vals) MemFree(vals);
        return false;
    }

    out->node   = (int)node;
    out->path   = (int)ch->target_path;
    out->interp = (int)ch->sampler->interpolation;
    out->keys   = (int)keys;
    out->comps  = comps;
    out->times  = times;
    out->vals   = vals;
    return true;
}

/* The file's inverse bind matrix for one joint, spec default (identity) when
 * the skin does not store them. Column-major in the file, like all of glTF. */
static Matrix joint_ibm(const cgltf_skin *skin, int b)
{
    cgltf_float raw[16];
    if (skin->inverse_bind_matrices &&
        cgltf_accessor_read_float(skin->inverse_bind_matrices, (cgltf_size)b,
                                  raw, 16))
        return mat_from_gltf(raw);
    return MatrixIdentity();
}

static Matrix node_world(const cgltf_node *n)
{
    cgltf_float raw[16];
    cgltf_node_transform_world(n, raw);
    return mat_from_gltf(raw);
}

/*
 * The conversion the skinning applies to raw vertices at rest:
 * jointWorld(rest) * IBM per bone, one constant for the whole of a consistent
 * rig. Robust pick rather than an average -- the bone closest to the
 * component-wise median wins, so one oddly-bound helper bone cannot tilt the
 * result -- because this drives up-axis detection and the height fit, where a
 * slightly wrong rotation reads as "lying down".
 */
static Matrix rest_conversion(const cgltf_skin *skin, int bones)
{
    /* Rotation/scale block and translation of each candidate. */
    enum { COMPS = 12 };
    static const int IDX[COMPS] = { 0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14 };

    Matrix *d   = MemAlloc((unsigned int)(bones * sizeof(Matrix)));
    float *vals = MemAlloc((unsigned int)(bones * sizeof(float)));
    if (!d || !vals) {
        if (d) MemFree(d);
        if (vals) MemFree(vals);
        return MatrixIdentity();
    }

    for (int b = 0; b < bones; b++)
        d[b] = MatrixMultiply(joint_ibm(skin, b), node_world(skin->joints[b]));

    float med[COMPS];
    for (int c = 0; c < COMPS; c++) {
        for (int b = 0; b < bones; b++) vals[b] = ((const float *)&d[b])[IDX[c]];
        for (int i = 1; i < bones; i++)         /* insertion sort; n is small */
            for (int j = i; j > 0 && vals[j] < vals[j - 1]; j--) {
                float t = vals[j]; vals[j] = vals[j - 1]; vals[j - 1] = t;
            }
        med[c] = vals[bones / 2];
    }

    int best = 0;
    float best_err = 0.0f;
    for (int b = 0; b < bones; b++) {
        float err = 0.0f;
        for (int c = 0; c < COMPS; c++)
            err += fabsf(((const float *)&d[b])[IDX[c]] - med[c]);
        if (b == 0 || err < best_err) { best = b; best_err = err; }
    }

    Matrix c = d[best];
    MemFree(d);
    MemFree(vals);
    return c;
}

ModelAnimation *anim_gltf_load(const char *path, ModelSkeleton *skeleton,
                               int *count, Matrix *rest_transform)
{
    *count = 0;
    *rest_transform = MatrixIdentity();
    if (!skeleton || skeleton->boneCount <= 0) return NULL;

    cgltf_options options = { 0 };
    cgltf_data *data = NULL;
    if (cgltf_parse_file(&options, path, &data) != cgltf_result_success)
        return NULL;

    /* Validate before touching buffer data: this is the bounds checking that
     * makes the rest of the module safe to run on an arbitrary file. */
    if (cgltf_load_buffers(&options, data, path) != cgltf_result_success ||
        cgltf_validate(data) != cgltf_result_success ||
        data->skins_count < 1 || data->animations_count < 1 ||
        data->skins[0].joints_count != (size_t)skeleton->boneCount) {
        cgltf_free(data);
        return NULL;
    }

    const cgltf_skin *skin = &data->skins[0];
    int bone_count = skeleton->boneCount;
    int node_count = (int)data->nodes_count;
    int anim_count = (int)data->animations_count;

    ModelAnimation *anims = MemAlloc((unsigned int)(anim_count * sizeof(ModelAnimation)));
    NodeAnim *node_anim   = MemAlloc((unsigned int)(node_count * sizeof(NodeAnim)));
    Matrix   *locals      = MemAlloc((unsigned int)(node_count * sizeof(Matrix)));
    Matrix   *cache       = MemAlloc((unsigned int)(node_count * sizeof(Matrix)));
    bool     *have        = MemAlloc((unsigned int)(node_count * sizeof(bool)));
    if (!anims || !node_anim || !locals || !cache || !have) goto fail;

    /* The fail path walks these looking for NULLs, so they must start zeroed
     * whatever the allocator's habits are. */
    memset(anims, 0, (size_t)anim_count * sizeof(ModelAnimation));

    for (int a = 0; a < anim_count; a++) {
        const cgltf_animation *src = &data->animations[a];
        ModelAnimation *dst = &anims[a];

        if (src->name) snprintf(dst->name, sizeof(dst->name), "%s", src->name);
        else snprintf(dst->name, sizeof(dst->name), "anim%d", a);
        dst->boneCount = bone_count;

        Track *tracks = MemAlloc((unsigned int)(src->channels_count * sizeof(Track)));
        if (!tracks && src->channels_count > 0) goto fail;

        int track_count = 0;
        float duration = 0.0f;
        for (int i = 0; i < node_count; i++)
            node_anim[i] = (NodeAnim){ -1, -1, -1 };

        for (size_t c = 0; c < src->channels_count; c++) {
            Track *tr = &tracks[track_count];
            if (!track_build(data, &src->channels[c], tr)) continue;

            /* Last writer wins on a duplicate channel, same as raylib. */
            NodeAnim *na = &node_anim[tr->node];
            if (tr->path == cgltf_animation_path_type_translation) na->t_track = track_count;
            else if (tr->path == cgltf_animation_path_type_rotation) na->r_track = track_count;
            else na->s_track = track_count;

            float end = tr->times[tr->keys - 1];
            if (end > duration) duration = end;
            track_count++;
        }

        int frames = (int)(duration * ANIM_GLTF_FPS) + 1;
        if (frames < 1) frames = 1;
        if (frames > MAX_KEYFRAMES) frames = MAX_KEYFRAMES;

        dst->keyframeCount = frames;
        dst->keyframePoses = MemAlloc((unsigned int)(frames * sizeof(Transform *)));
        if (!dst->keyframePoses) { free_tracks(tracks, track_count); goto fail; }
        memset(dst->keyframePoses, 0, (size_t)frames * sizeof(Transform *));

        for (int f = 0; f < frames; f++) {
            Transform *pose = MemAlloc((unsigned int)(bone_count * sizeof(Transform)));
            dst->keyframePoses[f] = pose;
            if (!pose) { free_tracks(tracks, track_count); goto fail; }

            float t = (float)f / ANIM_GLTF_FPS;

            for (int i = 0; i < node_count; i++) {
                locals[i] = node_local(&data->nodes[i], &node_anim[i], tracks, t);
                have[i] = false;
            }

            for (int b = 0; b < bone_count; b++) {
                Matrix g = node_global(data, skin->joints[b], locals, cache, have);
                MatrixDecompose(g, &pose[b].translation, &pose[b].rotation,
                                &pose[b].scale);
            }
        }

        free_tracks(tracks, track_count);
    }

    MemFree(node_anim);
    MemFree(locals);
    MemFree(cache);
    MemFree(have);

    /*
     * Every clip loaded. Now rewrite the bind pose so the runtime's
     * inv(bindPose) reproduces the file's inverse bind matrix exactly, which
     * with the node-space poses above makes boneMatrix the spec's
     * jointWorld(t) * IBM. Committed only here, after nothing can fail: an
     * import that falls back to raylib's loader must keep raylib's bind pose,
     * because those two disagree in exactly the way this module exists to fix.
     */
    for (int b = 0; b < bone_count; b++)
        MatrixDecompose(MatrixInvert(joint_ibm(skin, b)),
                        &skeleton->bindPose[b].translation,
                        &skeleton->bindPose[b].rotation,
                        &skeleton->bindPose[b].scale);

    *rest_transform = rest_conversion(skin, bone_count);

    cgltf_free(data);
    *count = anim_count;
    return anims;

fail:
    if (anims) anim_gltf_unload(anims, anim_count);
    if (node_anim) MemFree(node_anim);
    if (locals) MemFree(locals);
    if (cache) MemFree(cache);
    if (have) MemFree(have);
    cgltf_free(data);
    return NULL;
}
