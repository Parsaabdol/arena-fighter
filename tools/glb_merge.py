"""
Merge Source 2 hero exports into a single .glb under ONE armature.

Dota ships a hero as a body model plus one model per item slot (belt, cape,
weapon...). Source2Viewer-CLI exports each to its own .glb, each carrying a
small skin whose joints are a NAME-subset of the hero skeleton and whose
animations are absent. Loading only the body is why an export looks stripped.

This grafts every item mesh onto the body's skeleton:

  * one skin survives -- the body's -- so a loader that keys off skins[0] sees
    every mesh,
  * JOINTS_0 indices are remapped body-side by BONE NAME,
  * inverse bind matrices are compared before the graft and a mismatch is a
    hard error, because a differing bind pose cannot be fixed by remapping
    (skinning blends several joints per vertex, so there is no single
    correction to bake in).

Usage:
  glb_merge.py body.glb item.glb [item.glb ...] -o out.glb [--drop-mesh SUBSTR]
"""
import argparse
import base64
import json
import os
import struct
from urllib.parse import unquote

GLB_MAGIC = 0x46546C67
CHUNK_JSON = 0x4E4F534A
CHUNK_BIN = 0x004E4942

# Relative, because a Source 2 rig is authored in inches: the translation column
# of an inverse bind matrix runs to hundreds while the rotation part is order 1,
# so one absolute epsilon cannot serve both. Body and item are exported from the
# same rest pose, and what survives is float32 rounding -- around 1e-6 relative,
# four orders under this.
IBM_REL_TOLERANCE = 1e-3


class GraftError(Exception):
    """This item cannot be attached to this body. Raised rather than exiting:
    one odd slot out of eight should cost you that slot, not the hero."""


# ----------------------------------------------------------------- glb io ---

def read_glb(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, _ver, total = struct.unpack_from("<III", data, 0)
    if magic != GLB_MAGIC:
        raise SystemExit("%s: not a glb" % path)
    off, js, bin_chunk = 12, None, b""
    while off < total:
        clen, ctype = struct.unpack_from("<II", data, off)
        chunk = data[off + 8: off + 8 + clen]
        if ctype == CHUNK_JSON:
            js = json.loads(chunk.decode("utf-8"))
        elif ctype == CHUNK_BIN:
            bin_chunk = chunk
        off += 8 + clen
        off += (4 - off % 4) % 4
    return js, bin_chunk


def write_glb(path, gltf, blob):
    js = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    js += b" " * ((4 - len(js) % 4) % 4)
    blob += b"\0" * ((4 - len(blob) % 4) % 4)
    total = 12 + 8 + len(js) + (8 + len(blob) if blob else 0)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", GLB_MAGIC, 2, total))
        f.write(struct.pack("<II", len(js), CHUNK_JSON))
        f.write(js)
        if blob:
            f.write(struct.pack("<II", len(blob), CHUNK_BIN))
            f.write(blob)


# ------------------------------------------------------------- accessor io ---

COMP_SIZE = {5120: 1, 5121: 1, 5122: 2, 5123: 2, 5125: 4, 5126: 4}
COMP_FMT = {5120: "b", 5121: "B", 5122: "h", 5123: "H", 5125: "I", 5126: "f"}
TYPE_COUNT = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4,
              "MAT2": 4, "MAT3": 9, "MAT4": 16}


def read_accessor(gltf, blob, index):
    """Return a list of tuples (or scalars) for an accessor. No sparse support."""
    acc = gltf["accessors"][index]
    if "sparse" in acc:
        raise SystemExit("sparse accessors are not supported")
    n = TYPE_COUNT[acc["type"]]
    fmt = COMP_FMT[acc["componentType"]]
    esize = COMP_SIZE[acc["componentType"]] * n
    if "bufferView" not in acc:
        return [(0,) * n] * acc["count"]
    bv = gltf["bufferViews"][acc["bufferView"]]
    stride = bv.get("byteStride") or esize
    base = bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
    out = []
    for i in range(acc["count"]):
        vals = struct.unpack_from("<" + fmt * n, blob, base + i * stride)
        out.append(vals if n > 1 else vals[0])
    return out


def joint_names(gltf, skin_index):
    skin = gltf["skins"][skin_index]
    return [gltf["nodes"][j].get("name") for j in skin["joints"]]


# ------------------------------------------------------------------- mat4 ---
# glTF stores a matrix as 16 floats in COLUMN-major order, so element (row r,
# column c) lives at c * 4 + r. Everything here keeps that layout.

IDENTITY4 = (1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0)


def mat_mul(a, b):
    """a * b, both column-major."""
    out = [0.0] * 16
    for c in range(4):
        for r in range(4):
            out[c * 4 + r] = sum(a[k * 4 + r] * b[c * 4 + k] for k in range(4))
    return tuple(out)


def mat_inverse(m):
    """Gauss-Jordan. Small and general -- these are 4x4 done once per joint."""
    a = [[m[c * 4 + r] for c in range(4)] + [1.0 if i == r else 0.0
                                             for i in range(4)]
         for r in range(4)]
    for col in range(4):
        piv = max(range(col, 4), key=lambda r: abs(a[r][col]))
        if abs(a[piv][col]) < 1e-12:
            raise SystemExit("singular bind matrix")
        a[col], a[piv] = a[piv], a[col]
        f = a[col][col]
        a[col] = [v / f for v in a[col]]
        for r in range(4):
            if r != col and a[r][col] != 0.0:
                f = a[r][col]
                a[r] = [v - f * w for v, w in zip(a[r], a[col])]
    return tuple(a[r][4 + c] for c in range(4) for r in range(4))


def xform_point(m, p):
    return tuple(
        m[0 * 4 + r] * p[0] + m[1 * 4 + r] * p[1] + m[2 * 4 + r] * p[2]
        + m[3 * 4 + r] for r in range(3))


def xform_dir(m, v):
    return tuple(
        m[0 * 4 + r] * v[0] + m[1 * 4 + r] * v[1] + m[2 * 4 + r] * v[2]
        for r in range(3))


def mat_close(a, b, tol=1e-4):
    return all(abs(x - y) <= tol * max(1.0, abs(y)) for x, y in zip(a, b))


# ---------------------------------------------------------------- merging ---

def align4(blob):
    pad = (4 - len(blob) % 4) % 4
    return blob + b"\0" * pad


class Merger:
    def __init__(self, gltf, blob):
        self.g = gltf
        self.blob = bytearray(blob)
        # Everything below indexes into self.g; a merge appends and never
        # reorders, so indices already handed out stay valid.

    def add_bytes(self, data):
        self.blob = bytearray(align4(bytes(self.blob)))
        off = len(self.blob)
        self.blob += data
        return off

    def import_buffer_view(self, src_g, src_blob, bv_index):
        bv = dict(src_g["bufferViews"][bv_index])
        start = bv.get("byteOffset", 0)
        data = src_blob[start:start + bv["byteLength"]]
        bv["byteOffset"] = self.add_bytes(data)
        bv["buffer"] = 0
        self.g["bufferViews"].append(bv)
        return len(self.g["bufferViews"]) - 1

    def import_accessor(self, src_g, src_blob, acc_index, cache):
        if acc_index in cache:
            return cache[acc_index]
        acc = dict(src_g["accessors"][acc_index])
        if "bufferView" in acc:
            acc["bufferView"] = self.import_buffer_view(
                src_g, src_blob, acc["bufferView"])
        self.g["accessors"].append(acc)
        cache[acc_index] = len(self.g["accessors"]) - 1
        return cache[acc_index]

    def add_accessor_from_values(self, values, comp_type, gltf_type,
                                 bounds=False):
        """Write a fresh accessor; used for remapped joints and baked vertices."""
        n = TYPE_COUNT[gltf_type]
        fmt = "<" + COMP_FMT[comp_type] * n
        data = b"".join(struct.pack(fmt, *v) for v in values)
        off = self.add_bytes(data)
        self.g["bufferViews"].append(
            {"buffer": 0, "byteOffset": off, "byteLength": len(data)})
        acc = {
            "bufferView": len(self.g["bufferViews"]) - 1,
            "componentType": comp_type,
            "count": len(values),
            "type": gltf_type,
        }
        if bounds and values:
            # POSITION must carry min/max per the spec; viewers use it to frame
            # the model and some importers reject an accessor without it.
            acc["min"] = [min(v[k] for v in values) for k in range(n)]
            acc["max"] = [max(v[k] for v in values) for k in range(n)]
        self.g["accessors"].append(acc)
        return len(self.g["accessors"]) - 1

    def import_image(self, src_g, src_blob, img_index, cache, src_dir):
        if img_index in cache:
            return cache[img_index]
        img = dict(src_g["images"][img_index])
        if img.get("uri") and not img["uri"].startswith("data:"):
            # Make it absolute now, while we still know which file it came from;
            # embed_external() resolves the lot in one pass at the end.
            img["uri"] = os.path.join(src_dir, unquote(img["uri"]))
        if "bufferView" in img:
            img["bufferView"] = self.import_buffer_view(
                src_g, src_blob, img["bufferView"])
        elif "uri" in img and img["uri"].startswith("data:"):
            payload = base64.b64decode(img["uri"].split(",", 1)[1])
            off = self.add_bytes(payload)
            self.g["bufferViews"].append(
                {"buffer": 0, "byteOffset": off, "byteLength": len(payload)})
            img.pop("uri")
            img["bufferView"] = len(self.g["bufferViews"]) - 1
        self.g.setdefault("images", []).append(img)
        cache[img_index] = len(self.g["images"]) - 1
        return cache[img_index]

    def import_texture(self, src_g, src_blob, tex_index, caches):
        if tex_index in caches["tex"]:
            return caches["tex"][tex_index]
        tex = dict(src_g["textures"][tex_index])
        if "source" in tex:
            tex["source"] = self.import_image(
                src_g, src_blob, tex["source"], caches["img"], caches["dir"])
        if "sampler" in tex:
            key = tex["sampler"]
            if key not in caches["smp"]:
                self.g.setdefault("samplers", []).append(
                    dict(src_g["samplers"][key]))
                caches["smp"][key] = len(self.g["samplers"]) - 1
            tex["sampler"] = caches["smp"][key]
        self.g.setdefault("textures", []).append(tex)
        caches["tex"][tex_index] = len(self.g["textures"]) - 1
        return caches["tex"][tex_index]

    def _remap_tex_refs(self, node, src_g, src_blob, caches):
        """Walk a material subtree rewriting every {"index": n} texture ref."""
        if isinstance(node, dict):
            for k, v in list(node.items()):
                if k.endswith("Texture") or k in (
                        "diffuseTexture", "specularGlossinessTexture"):
                    if isinstance(v, dict) and "index" in v:
                        v["index"] = self.import_texture(
                            src_g, src_blob, v["index"], caches)
                else:
                    self._remap_tex_refs(v, src_g, src_blob, caches)
        elif isinstance(node, list):
            for v in node:
                self._remap_tex_refs(v, src_g, src_blob, caches)

    def import_material(self, src_g, src_blob, mat_index, caches):
        if mat_index in caches["mat"]:
            return caches["mat"][mat_index]
        mat = json.loads(json.dumps(src_g["materials"][mat_index]))
        self._remap_tex_refs(mat, src_g, src_blob, caches)
        self.g.setdefault("materials", []).append(mat)
        caches["mat"][mat_index] = len(self.g["materials"]) - 1
        return caches["mat"][mat_index]


def embed_external(m):
    """Pull every sidecar texture into the .glb and drop duplicates.

    Source2Viewer writes textures next to the model and points at them with
    relative URIs. That is fine for a folder of exports and wrong for this
    game, where the whole workflow is "drop a .glb in assets/" -- a model that
    silently loses its textures when moved is worse than one that fails to
    load. Identical files are embedded once however many materials cite them.
    """
    by_path, freed = {}, 0
    remap = {}
    for i, img in enumerate(m.g.get("images", [])):
        uri = img.get("uri")
        if not uri or uri.startswith("data:"):
            continue
        path = os.path.abspath(uri)
        if path in by_path:
            # Point the duplicate at the bytes already in the file rather than
            # leaving it holding a URI: an entry nothing references is still an
            # entry a strict loader will try to fetch.
            canon = m.g["images"][by_path[path]]
            img.pop("uri")
            img["bufferView"] = canon["bufferView"]
            img["mimeType"] = canon["mimeType"]
            remap[i] = by_path[path]
            freed += 1
            continue
        if not os.path.exists(path):
            raise SystemExit("texture not found: %s" % path)
        with open(path, "rb") as f:
            data = f.read()
        off = m.add_bytes(data)
        m.g["bufferViews"].append(
            {"buffer": 0, "byteOffset": off, "byteLength": len(data)})
        img.pop("uri")
        img["bufferView"] = len(m.g["bufferViews"]) - 1
        img.setdefault("mimeType",
                       "image/jpeg" if path.lower().endswith((".jpg", ".jpeg"))
                       else "image/png")
        by_path[path] = i
        remap[i] = i

    if remap:
        for tex in m.g.get("textures", []):
            if "source" in tex and tex["source"] in remap:
                tex["source"] = remap[tex["source"]]
    return len(by_path), freed


# ------------------------------------------------------------------ checks ---

def bind_corrections(dst_g, dst_blob, src_g, src_blob, joint_index):
    """Per source joint: the matrix that rebases its vertices onto the body.

    A shared joint usually carries the same inverse bind matrix in both files
    and needs nothing. Rigid props are the exception -- Source 2 authors a
    weapon at the origin and gives it an item-local bind pose -- so for those
    the vertices must move instead. Skinning evaluates

        world = jointWorld_j * IBM_j * v

    so preserving the result while swapping in the body's IBM means

        v' = dst_IBM_j^-1 * src_IBM_j * v

    which is exact for any vertex whose influences all share one correction,
    and undefined for one that blends two different ones.
    """
    dst_ibm = read_accessor(dst_g, dst_blob,
                            dst_g["skins"][0]["inverseBindMatrices"])
    src_ibm = read_accessor(src_g, src_blob,
                            src_g["skins"][0]["inverseBindMatrices"])
    names = joint_names(src_g, 0)
    corrections, worst, worst_bone = [], 0.0, None
    for i, nm in enumerate(names):
        di = joint_index(nm)
        rel = max(abs(a - b) / max(1.0, abs(b))
                  for a, b in zip(src_ibm[i], dst_ibm[di]))
        if rel > worst:
            worst, worst_bone = rel, nm
        if rel <= IBM_REL_TOLERANCE:
            corrections.append(IDENTITY4)
        else:
            corrections.append(mat_mul(mat_inverse(dst_ibm[di]), src_ibm[i]))
    return corrections, worst, worst_bone


def bake_corrections(prim_attrs, src_g, src_blob, joints, weights, corrections):
    """Return rebased POSITION/NORMAL/TANGENT lists, or None if nothing moved.

    Raises if a vertex blends joints whose corrections disagree -- that cannot
    be expressed by moving vertices and means the two rigs genuinely differ.
    """
    if all(c is IDENTITY4 for c in corrections):
        return None
    per_vertex = []
    for ji, wi in zip(joints, weights):
        chosen = None
        for j, w in zip(ji, wi):
            if w <= 1e-6:
                continue
            c = corrections[j]
            if chosen is None:
                chosen = c
            elif not mat_close(chosen, c):
                raise GraftError(
                    "a vertex blends joints with different bind poses; the "
                    "item rig is not a rigid attachment")
        per_vertex.append(chosen if chosen is not None else IDENTITY4)

    out = {}
    pos = read_accessor(src_g, src_blob, prim_attrs["POSITION"])
    out["POSITION"] = [xform_point(c, p) for c, p in zip(per_vertex, pos)]
    for attr in ("NORMAL", "TANGENT"):
        if attr not in prim_attrs:
            continue
        vals = read_accessor(src_g, src_blob, prim_attrs[attr])
        # These corrections are rigid (rotation + translation), so a direction
        # transforms by the same basis; no inverse-transpose needed.
        if attr == "TANGENT":
            out[attr] = [xform_dir(c, v[:3]) + (v[3],)
                         for c, v in zip(per_vertex, vals)]
        else:
            out[attr] = [xform_dir(c, v) for c, v in zip(per_vertex, vals)]
    return out


# ------------------------------------------------------------------- merge ---

def merge_files(body, items, output, drop_mesh=()):
    """Graft every item onto the body's skeleton and write one self-contained
    .glb. This is the whole module's reason to exist; main() only parses for
    it. Returns the number of meshes written."""
    say = print

    g, blob = read_glb(body)
    if not g.get("skins"):
        raise SystemExit("%s has no skin -- export with --gltf_export_animations"
                         % body)

    # Collapse the body's own skins first. Source2Viewer emits one skin per
    # mesh; when they share a joint list they are one armature already and the
    # duplicates only confuse a loader that reads skins[0].
    base_joints = g["skins"][0]["joints"]
    for i, s in enumerate(g["skins"][1:], 1):
        if s["joints"] != base_joints:
            raise SystemExit("body skin %d has a different joint list" % i)
    for node in g["nodes"]:
        if "skin" in node:
            node["skin"] = 0
    g["skins"] = g["skins"][:1]
    say("body: %d joints, %d animations, %d mesh(es)" % (
        len(base_joints), len(g.get("animations", [])), len(g["meshes"])))

    # Bone lookup is CASE-INSENSITIVE. Valve's own item models disagree with
    # the body about capitalisation -- Necrophos's hat is rigged to `spine_3`
    # and `head_0` where his body calls them `Spine_3` and `Head_0` -- and they
    # are plainly the same bones. Exact names still win where a rig really does
    # distinguish two bones by case alone.
    name_to_dst = {}
    for i, nm in enumerate(joint_names(g, 0)):
        name_to_dst[nm] = i
    folded = {}
    for nm, i in name_to_dst.items():
        key = (nm or "").lower()
        folded[key] = None if key in folded else i   # None marks a collision

    def joint_index(nm):
        if nm in name_to_dst:
            return name_to_dst[nm]
        return folded.get((nm or "").lower())

    m = Merger(g, blob)
    scene = g["scenes"][g.get("scene", 0)]

    # The body's own textures are relative to the body file; make them absolute
    # on the same terms as every imported one.
    body_dir = os.path.dirname(os.path.abspath(body))
    for img in g.get("images", []):
        if img.get("uri") and not img["uri"].startswith("data:"):
            img["uri"] = os.path.join(body_dir, unquote(img["uri"]))

    for path in items:
        label = os.path.basename(path)
        src_g, src_blob = read_glb(path)
        if not src_g.get("skins"):
            say("  skip  %-24s no skin" % label)
            continue
        names = joint_names(src_g, 0)
        missing = [n for n in names if joint_index(n) is None]
        if missing:
            # Not everything in a hero's folder is rigged to that hero. Some
            # props carry their own little skeleton -- Necrophos's Reaper's
            # Scythe is root/bind/pivot/anim -- and are placed at runtime by
            # attachment rather than skinned. Nothing here can graft one, and
            # losing the rest of the hero over it would be the worse trade.
            say("  skip  %-24s rigged to a different skeleton (%s)"
                % (label, ", ".join(missing[:4])))
            continue

        try:
            corrections, worst, worst_bone = bind_corrections(
                g, bytes(m.blob), src_g, src_blob, joint_index)
        except GraftError as e:
            say("  skip  %-24s %s" % (label, e))
            continue
        rebased = sum(1 for c in corrections if c is not IDENTITY4)
        if rebased:
            say("  bind  %-24s %d/%d bone(s) rebased (worst %s, %.1e)"
                  % (path.split("\\")[-1], rebased, len(corrections),
                     worst_bone, worst))
        else:
            say("  bind  %-24s shared, worst drift %.1e relative"
                  % (path.split("\\")[-1], worst))

        remap = [joint_index(n) for n in names]
        caches = {"img": {}, "tex": {}, "smp": {}, "mat": {}, "acc": {},
                  "dir": os.path.dirname(os.path.abspath(path))}

        for mesh in src_g["meshes"]:
            mname = mesh.get("name", "")
            if any(d.lower() in mname.lower() for d in drop_mesh):
                say("  drop  %s" % mname.split(".")[-1])
                continue
            new_prims = []
            for prim in mesh["primitives"]:
                np_ = {"attributes": {}}
                attrs = prim["attributes"]
                joints = read_accessor(src_g, src_blob, attrs["JOINTS_0"])
                weights = read_accessor(src_g, src_blob, attrs["WEIGHTS_0"])
                try:
                    baked = bake_corrections(attrs, src_g, src_blob,
                                             joints, weights, corrections)
                except GraftError as e:
                    say("  skip  %-24s %s" % (label, e))
                    new_prims = None
                    break
                for attr, acc_i in attrs.items():
                    if attr.startswith("JOINTS_"):
                        acc = src_g["accessors"][acc_i]
                        vals = (joints if attr == "JOINTS_0"
                                else read_accessor(src_g, src_blob, acc_i))
                        moved = [tuple(remap[v] for v in tup) for tup in vals]
                        np_["attributes"][attr] = m.add_accessor_from_values(
                            moved, acc["componentType"], acc["type"])
                    elif baked and attr in baked:
                        acc = src_g["accessors"][acc_i]
                        np_["attributes"][attr] = m.add_accessor_from_values(
                            baked[attr], 5126, acc["type"],
                            bounds=(attr == "POSITION"))
                    else:
                        np_["attributes"][attr] = m.import_accessor(
                            src_g, src_blob, acc_i, caches["acc"])
                if "indices" in prim:
                    np_["indices"] = m.import_accessor(
                        src_g, src_blob, prim["indices"], caches["acc"])
                if "material" in prim:
                    np_["material"] = m.import_material(
                        src_g, src_blob, prim["material"], caches)
                if "mode" in prim:
                    np_["mode"] = prim["mode"]
                new_prims.append(np_)
            if new_prims is None:            # the bake gave up on this mesh
                continue
            g["meshes"].append({"name": mname, "primitives": new_prims})
            g["nodes"].append({
                "name": mname.split(".")[-1] or ("mesh%d" % len(g["nodes"])),
                "mesh": len(g["meshes"]) - 1,
                "skin": 0,
            })
            scene["nodes"].append(len(g["nodes"]) - 1)
            verts = sum(src_g["accessors"][p["attributes"]["POSITION"]]["count"]
                        for p in mesh["primitives"])
            say("  graft %-28s %5d verts over %2d bones"
                  % (mname.split(".")[-1], verts, len(names)))

        for key in ("extensionsUsed", "extensionsRequired"):
            if key in src_g:
                merged = set(g.get(key, [])) | set(src_g[key])
                g[key] = sorted(merged)

    embedded, deduped = embed_external(m)
    say("  images: %d embedded, %d duplicate reference(s) collapsed"
          % (embedded, deduped))

    g["buffers"] = [{"byteLength": len(align4(bytes(m.blob)))}]
    write_glb(output, g, bytes(m.blob))
    say("wrote %s  (%d meshes, 1 skin, %d animations, self-contained)"
          % (output, len(g["meshes"]), len(g.get("animations", []))))
    return len(g["meshes"])


# -------------------------------------------------------------------- main ---

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("body")
    ap.add_argument("items", nargs="*")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--drop-mesh", action="append", default=[],
                    help="skip any mesh whose name contains this substring")
    args = ap.parse_args()
    merge_files(args.body, args.items, args.output, args.drop_mesh)


if __name__ == "__main__":
    main()
