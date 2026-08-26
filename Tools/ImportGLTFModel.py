#!/usr/bin/env python3
"""
Compiles a glTF/GLB model into a Source .mdl, so models authored anywhere can be loaded by an engine that only
speaks Source's formats.

The engine reads .mdl/.vvd/.vtx at runtime and nothing else. Rather than teach it a second model format, this
takes the long way round and produces a real Source model: the mesh and skeleton become .smd, each animation
becomes another .smd, the embedded textures become .vtf with .vmt beside them, a .qc ties them together, and the
Source SDK's own studiomdl compiles the lot. What comes out is indistinguishable from a model made in Blender
and compiled the usual way, because that is exactly what it is.

    python Tools/ImportGLTFModel.py model.glb --name player/gordon --out <moddir>
    python Tools/ImportGLTFModel.py legs.glb  --name player/gordon_legs --out <moddir> --no-textures

Axes: glTF is Y-up with -Z forward; Source is Z-up with +X forward. Every position, normal and bone pose goes
through that change of basis - for the bone poses as a conjugation, M * G * M^-1, because a rotation has to be
re-expressed in the new basis rather than merely relabelled.

Scale: none by default. A model authored against Source's 72-unit player is already in Source units; --scale is
there for one authored in metres.
"""
import argparse
import base64
import json
import math
import os
import struct
import subprocess
import sys

# glTF component types -> (struct code, byte size)
COMPONENT = {
    5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2),
    5123: ("H", 2), 5125: ("I", 4), 5126: ("f", 4),
}
NUM_COMPONENTS = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}

FPS = 30.0


# ----------------------------------------------------------------------------------------------- small matrix math
def mat_identity():
    return [[1.0 if i == j else 0.0 for j in range(4)] for i in range(4)]


def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def mat_from_trs(t, r, s):
    x, y, z, w = r
    xx, yy, zz = x * x, y * y, z * z
    xy, xz, yz = x * y, x * z, y * z
    wx, wy, wz = w * x, w * y, w * z
    rot = [
        [1 - 2 * (yy + zz), 2 * (xy - wz), 2 * (xz + wy)],
        [2 * (xy + wz), 1 - 2 * (xx + zz), 2 * (yz - wx)],
        [2 * (xz - wy), 2 * (yz + wx), 1 - 2 * (xx + yy)],
    ]
    m = mat_identity()
    for i in range(3):
        for j in range(3):
            m[i][j] = rot[i][j] * s[j]
        m[i][3] = t[i]
    return m


def mat_inverse(m):
    # Rigid-plus-scale inverse via cofactors; general enough for anything a skeleton carries.
    a = [row[:] for row in m]
    inv = mat_identity()
    for col in range(4):
        pivot = max(range(col, 4), key=lambda r: abs(a[r][col]))
        if abs(a[pivot][col]) < 1e-12:
            continue
        a[col], a[pivot] = a[pivot], a[col]
        inv[col], inv[pivot] = inv[pivot], inv[col]
        d = a[col][col]
        a[col] = [v / d for v in a[col]]
        inv[col] = [v / d for v in inv[col]]
        for r in range(4):
            if r == col:
                continue
            f = a[r][col]
            if f == 0.0:
                continue
            a[r] = [av - f * bv for av, bv in zip(a[r], a[col])]
            inv[r] = [av - f * bv for av, bv in zip(inv[r], inv[col])]
    return inv


def mat_apply_point(m, p):
    return [m[i][0] * p[0] + m[i][1] * p[1] + m[i][2] * p[2] + m[i][3] for i in range(3)]


def mat_apply_dir(m, p):
    return [m[i][0] * p[0] + m[i][1] * p[1] + m[i][2] * p[2] for i in range(3)]


# glTF -> Source basis: sx = -gz, sy = -gx, sz = gy.
BASIS = [
    [0.0, 0.0, -1.0, 0.0],
    [-1.0, 0.0, 0.0, 0.0],
    [0.0, 1.0, 0.0, 0.0],
    [0.0, 0.0, 0.0, 1.0],
]
BASIS_INV = mat_inverse(BASIS)


def to_source_matrix(m):
    """A transform expressed in glTF axes, re-expressed in Source's."""
    return mat_mul(mat_mul(BASIS, m), BASIS_INV)


def strip_scale(m):
    """
    The same transform with its scale divided out, translation untouched.

    Rigs carry scale on the armature - Mixamo's is 0.01, because its joints are authored in centimetres and
    scaled down at the root. That scale is already baked into every global position, so taking a local as
    inv(parent) @ child divides the offset by it and hands back the pre-scale number: a spine 4.3 units above
    the hips comes out as 434. The reference pose and the animations agree on that error, which is why the
    model stands correctly and only falls apart once a sequence plays. SMD has no notion of bone scale, so the
    scale is removed here and every bone offset is in final units.
    """
    out = [row[:] for row in m]
    for j in range(3):
        n = math.sqrt(sum(m[i][j] * m[i][j] for i in range(3))) or 1.0
        for i in range(3):
            out[i][j] = m[i][j] / n
    return out


def decompose(m):
    """Translation and SMD euler radians, where the pose matrix is Rz(z) @ Ry(y) @ Rx(x) (mathlib AngleMatrix)."""
    t = [m[0][3], m[1][3], m[2][3]]
    # Strip any scale so the rotation extraction is well conditioned.
    cols = []
    for j in range(3):
        v = [m[0][j], m[1][j], m[2][j]]
        n = math.sqrt(sum(c * c for c in v)) or 1.0
        cols.append([c / n for c in v])
    r = [[cols[j][i] for j in range(3)] for i in range(3)]

    sy = max(-1.0, min(1.0, -r[2][0]))
    y = math.asin(sy)
    if abs(math.cos(y)) > 1e-6:
        x = math.atan2(r[2][1], r[2][2])
        z = math.atan2(r[1][0], r[0][0])
    else:
        x = math.atan2(-r[1][2], r[1][1])
        z = 0.0
    return t, [x, y, z]


# ----------------------------------------------------------------------------------------------- glTF reading
class GLTF:
    def __init__(self, path):
        self.dir = os.path.dirname(os.path.abspath(path))
        with open(path, "rb") as f:
            data = f.read()
        if data[:4] == b"glTF":
            _, _, _ = struct.unpack_from("<III", data, 0)
            off, self.bin = 12, b""
            while off < len(data):
                clen, ctype = struct.unpack_from("<II", data, off)
                chunk = data[off + 8:off + 8 + clen]
                if ctype == 0x4E4F534A:
                    self.j = json.loads(chunk.decode("utf-8"))
                elif ctype == 0x004E4942:
                    self.bin = chunk
                off += 8 + clen + ((4 - clen % 4) % 4 if clen % 4 else 0)
        else:
            self.j = json.loads(data.decode("utf-8"))
            self.bin = b""

    def buffer(self, index):
        buf = self.j["buffers"][index]
        uri = buf.get("uri")
        if uri is None:
            return self.bin
        if uri.startswith("data:"):
            return base64.b64decode(uri.split(",", 1)[1])
        with open(os.path.join(self.dir, uri), "rb") as f:
            return f.read()

    def accessor(self, index):
        acc = self.j["accessors"][index]
        n = NUM_COMPONENTS[acc["type"]]
        code, size = COMPONENT[acc["componentType"]]
        count = acc["count"]
        if "bufferView" not in acc:
            return [[0] * n for _ in range(count)]
        view = self.j["bufferViews"][acc["bufferView"]]
        data = self.buffer(view.get("buffer", 0))
        base = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
        stride = view.get("byteStride") or (n * size)

        out = []
        for i in range(count):
            o = base + i * stride
            out.append(list(struct.unpack_from("<" + code * n, data, o)))
        if acc.get("normalized"):
            denom = {5120: 127.0, 5121: 255.0, 5122: 32767.0, 5123: 65535.0}.get(acc["componentType"])
            if denom:
                out = [[max(-1.0, v / denom) for v in row] for row in out]
        return out

    def image_bytes(self, index):
        img = self.j["images"][index]
        if "bufferView" in img:
            view = self.j["bufferViews"][img["bufferView"]]
            data = self.buffer(view.get("buffer", 0))
            base = view.get("byteOffset", 0)
            return data[base:base + view["byteLength"]]
        uri = img.get("uri", "")
        if uri.startswith("data:"):
            return base64.b64decode(uri.split(",", 1)[1])
        with open(os.path.join(self.dir, uri), "rb") as f:
            return f.read()


# ----------------------------------------------------------------------------------------------- skeleton
class Skeleton:
    """The joints of the first skin, flattened to the parent-indexed list an SMD wants."""

    def __init__(self, g):
        self.g = g
        skin = g.j["skins"][0]
        self.joints = skin["joints"]
        self.index_of = {n: i for i, n in enumerate(self.joints)}

        parent_of = {}
        for ni, node in enumerate(g.j["nodes"]):
            for c in node.get("children", []):
                parent_of[c] = ni
        self.parent = []
        for n in self.joints:
            p = parent_of.get(n)
            self.parent.append(self.index_of.get(p, -1) if p is not None else -1)

        self.names = []
        for i, n in enumerate(g.j["nodes"]):
            pass
        for n in self.joints:
            self.names.append(g.j["nodes"][n].get("name") or f"bone_{n}")

        # The bind pose, from the inverse bind matrices - the pose the mesh vertices were authored in.
        ibm = g.accessor(skin["inverseBindMatrices"]) if "inverseBindMatrices" in skin else None
        self.bind_global = []
        for i in range(len(self.joints)):
            if ibm:
                col = ibm[i]
                m = [[col[c * 4 + r] for c in range(4)] for r in range(4)]  # glTF matrices are column-major
                self.bind_global.append(mat_inverse(m))
            else:
                self.bind_global.append(self.node_global(self.joints[i]))

    def node_local(self, ni, pose=None):
        node = self.g.j["nodes"][ni]
        if pose and ni in pose:
            t, r, s = pose[ni]
        else:
            t = node.get("translation", [0.0, 0.0, 0.0])
            r = node.get("rotation", [0.0, 0.0, 0.0, 1.0])
            s = node.get("scale", [1.0, 1.0, 1.0])
            if "matrix" in node:
                col = node["matrix"]
                return [[col[c * 4 + r_] for c in range(4)] for r_ in range(4)]
        return mat_from_trs(t, r, s)

    def node_global(self, ni, pose=None):
        parent_of = getattr(self, "_parent_of", None)
        if parent_of is None:
            parent_of = {}
            for i, node in enumerate(self.g.j["nodes"]):
                for c in node.get("children", []):
                    parent_of[c] = i
            self._parent_of = parent_of
        m = self.node_local(ni, pose)
        p = parent_of.get(ni)
        while p is not None:
            m = mat_mul(self.node_local(p, pose), m)
            p = parent_of.get(p)
        return m

    def locals_from_globals(self, globals_source):
        """Source-space globals -> the local pose each bone is written as in an SMD."""
        flat = [strip_scale(m) for m in globals_source]
        out = []
        for i in range(len(self.joints)):
            p = self.parent[i]
            local = flat[i] if p < 0 else mat_mul(mat_inverse(flat[p]), flat[i])
            out.append(decompose(local))
        return out

    def bind_locals(self):
        return self.locals_from_globals([to_source_matrix(m) for m in self.bind_global])


# ----------------------------------------------------------------------------------------------- animation
def sample_animation(g, skel, anim):
    """Every joint's local pose, per frame, at FPS."""
    channels = {}
    duration = 0.0
    for ch in anim["channels"]:
        target = ch["target"]
        node, path = target.get("node"), target["path"]
        if node is None or path == "weights":
            continue
        sampler = anim["samplers"][ch["sampler"]]
        times = [t[0] for t in g.accessor(sampler["input"])]
        values = g.accessor(sampler["output"])
        if times:
            duration = max(duration, times[-1])
        channels.setdefault(node, {})[path] = (times, values, sampler.get("interpolation", "LINEAR"))

    frames = max(1, int(round(duration * FPS)) + 1)
    out = []
    for f in range(frames):
        t = f / FPS
        pose = {}
        for node, paths in channels.items():
            node_def = g.j["nodes"][node]
            tr = node_def.get("translation", [0.0, 0.0, 0.0])
            rot = node_def.get("rotation", [0.0, 0.0, 0.0, 1.0])
            sc = node_def.get("scale", [1.0, 1.0, 1.0])
            if "translation" in paths:
                tr = sample_channel(paths["translation"], t, tr)
            if "rotation" in paths:
                rot = normalise_quat(sample_channel(paths["rotation"], t, rot, slerp=True))
            if "scale" in paths:
                sc = sample_channel(paths["scale"], t, sc)
            pose[node] = (tr, rot, sc)
        globals_source = [to_source_matrix(skel.node_global(n, pose)) for n in skel.joints]
        out.append(skel.locals_from_globals(globals_source))
    return out


def sample_channel(channel, t, default, slerp=False):
    times, values, interp = channel
    if not times:
        return default
    if t <= times[0]:
        return values[0]
    if t >= times[-1]:
        return values[-1]
    lo, hi = 0, len(times) - 1
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if times[mid] <= t:
            lo = mid
        else:
            hi = mid
    span = times[hi] - times[lo]
    u = 0.0 if span <= 0 else (t - times[lo]) / span
    if interp == "STEP":
        return values[lo]
    a, b = values[lo], values[hi]
    if slerp:
        return quat_slerp(a, b, u)
    return [av + (bv - av) * u for av, bv in zip(a, b)]


def normalise_quat(q):
    n = math.sqrt(sum(c * c for c in q)) or 1.0
    return [c / n for c in q]


def quat_slerp(a, b, u):
    d = sum(x * y for x, y in zip(a, b))
    if d < 0.0:
        b = [-x for x in b]
        d = -d
    if d > 0.9995:
        return normalise_quat([x + (y - x) * u for x, y in zip(a, b)])
    theta = math.acos(max(-1.0, min(1.0, d)))
    s = math.sin(theta)
    wa, wb = math.sin((1 - u) * theta) / s, math.sin(u * theta) / s
    return normalise_quat([x * wa + y * wb for x, y in zip(a, b)])


# ----------------------------------------------------------------------------------------------- SMD writing
def write_nodes(f, skel):
    f.write("version 1\nnodes\n")
    for i, name in enumerate(skel.names):
        f.write(f'{i} "{name}" {skel.parent[i]}\n')
    f.write("end\n")


def write_skeleton(f, poses_per_frame):
    f.write("skeleton\n")
    for frame, poses in enumerate(poses_per_frame):
        f.write(f"time {frame}\n")
        for i, (t, r) in enumerate(poses):
            f.write(f"{i} {t[0]:.6f} {t[1]:.6f} {t[2]:.6f} {r[0]:.6f} {r[1]:.6f} {r[2]:.6f}\n")
    f.write("end\n")


def write_reference_smd(path, g, skel, scale, material_names):
    bind = skel.bind_locals()
    with open(path, "w", encoding="ascii") as f:
        write_nodes(f, skel)
        write_skeleton(f, [bind])
        f.write("triangles\n")

        mesh = g.j["meshes"][0]
        for prim in mesh["primitives"]:
            attrs = prim["attributes"]
            pos = g.accessor(attrs["POSITION"])
            nrm = g.accessor(attrs["NORMAL"]) if "NORMAL" in attrs else [[0, 1, 0]] * len(pos)
            uv = g.accessor(attrs["TEXCOORD_0"]) if "TEXCOORD_0" in attrs else [[0, 0]] * len(pos)
            joints = g.accessor(attrs["JOINTS_0"]) if "JOINTS_0" in attrs else None
            weights = g.accessor(attrs["WEIGHTS_0"]) if "WEIGHTS_0" in attrs else None
            idx = [i[0] for i in g.accessor(prim["indices"])] if "indices" in prim else list(range(len(pos)))
            mat = material_names[prim.get("material", 0)]

            for tri in range(0, len(idx) - 2, 3):
                f.write(f"{mat}\n")
                for k in range(3):
                    v = idx[tri + k]
                    p = mat_apply_point(BASIS, [c * scale for c in pos[v]])
                    n = mat_apply_dir(BASIS, nrm[v])
                    u, vv = uv[v][0], uv[v][1]

                    links = []
                    if joints and weights:
                        for b, w in zip(joints[v], weights[v]):
                            if w > 0.0001:
                                links.append((int(b), float(w)))
                        links.sort(key=lambda x: -x[1])
                        links = links[:3]
                        total = sum(w for _, w in links) or 1.0
                        links = [(b, w / total) for b, w in links]
                    if not links:
                        links = [(0, 1.0)]

                    parent = links[0][0]
                    # SMD's V runs the other way up from glTF's.
                    line = (f"{parent} {p[0]:.6f} {p[1]:.6f} {p[2]:.6f} "
                            f"{n[0]:.6f} {n[1]:.6f} {n[2]:.6f} {u:.6f} {1.0 - vv:.6f} {len(links)}")
                    for b, w in links:
                        line += f" {b} {w:.6f}"
                    f.write(line + "\n")
        f.write("end\n")


def write_anim_smd(path, skel, poses_per_frame):
    with open(path, "w", encoding="ascii") as f:
        write_nodes(f, skel)
        write_skeleton(f, poses_per_frame)


# ----------------------------------------------------------------------------------------------- materials
def safe_name(s, fallback):
    s = (s or fallback).strip().lower()
    return "".join(c if c.isalnum() or c in "_-" else "_" for c in s)


def export_materials(g, out_dir, material_path, tools_dir, quiet=False):
    """Every material's base texture as a .vtf, with a .vmt beside it. Returns the per-material names."""
    names = []
    written = {}
    mats = g.j.get("materials", [{}])
    for mi, mat in enumerate(mats):
        name = safe_name(mat.get("name"), f"material_{mi}")
        names.append(name)

        pbr = mat.get("pbrMetallicRoughness", {})
        tex_index = pbr.get("baseColorTexture", {}).get("index")
        image_index = None
        if tex_index is not None:
            image_index = g.j["textures"][tex_index].get("source")

        vmt_path = os.path.join(out_dir, name + ".vmt")
        # Two materials often share one image - a left and a right eye off the same sheet. The texture is
        # written once, under the name of whichever material got there first, and the second material's VMT
        # points at that one rather than at a .vtf nobody wrote.
        texture_name = written.get(image_index, name) if image_index is not None else name
        if image_index is not None and image_index not in written:
            blob = g.image_bytes(image_index)
            png = os.path.join(out_dir, name + ".png")
            with open(png, "wb") as f:
                f.write(blob)
            # The engine's own importer turns it into a VTF.
            cmd = [sys.executable, os.path.join(tools_dir, "ImportImageToVTF.py"), png,
                   os.path.join(out_dir, name + ".vtf"), "--format", "dxt5", "--mips"]
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode != 0 and not quiet:
                sys.stderr.write(r.stdout + r.stderr)
            os.remove(png)
            written[image_index] = name

        if image_index is None:
            # A material with no base colour texture at all - glTF allows it, and this model has one on an eye.
            # Writing a VMT anyway would point $basetexture at a .vtf that was never meant to exist, so the
            # material is left absent and the engine falls back to its default, which is what it is for.
            if not quiet:
                print(f"  (no texture on material '{name}' - left to the engine's default)")
            continue

        with open(vmt_path, "w", encoding="ascii") as f:
            f.write('VertexLitGeneric\n{\n')
            f.write(f'\t"$basetexture" "{material_path}/{texture_name}"\n')
            f.write('\t"$model" "1"\n')
            # MASK is a real cutout and becomes $alphatest. BLEND is deliberately not honoured: exporters set it
            # on anything that merely has an alpha channel, and a translucent character sorts against itself
            # badly - a model that really is see-through wants its VMT edited by hand.
            if mat.get("alphaMode") == "MASK":
                f.write('\t"$alphatest" "1"\n')
                f.write(f'\t"$alphatestreference" "{mat.get("alphaCutoff", 0.5)}"\n')
            if mat.get("doubleSided"):
                f.write('\t"$nocull" "1"\n')
            f.write('}\n')
    return names


# ----------------------------------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("gltf")
    ap.add_argument("--name", required=True, help="model path without extension, e.g. player/gordon")
    ap.add_argument("--out", required=True, help="mod directory (the one holding models/ and materials/)")
    ap.add_argument("--scale", type=float, default=1.0)
    ap.add_argument("--studiomdl", help="studiomdl.exe (default: Source SDK Base 2013 Singleplayer)")
    ap.add_argument("--game", help="game dir for studiomdl (default: the SDK's hl2)")
    ap.add_argument("--no-textures", action="store_true", help="reuse the materials already on disk")
    ap.add_argument("--surfaceprop", default="flesh")
    args = ap.parse_args()

    sdk = "C:/Program Files (x86)/Steam/steamapps/common/Source SDK Base 2013 Singleplayer"
    studiomdl = args.studiomdl or os.path.join(sdk, "bin", "studiomdl.exe")
    game = args.game or os.path.join(sdk, "hl2")
    if not os.path.isfile(studiomdl):
        sys.exit(f"studiomdl not found at {studiomdl} - pass --studiomdl")

    tools_dir = os.path.dirname(os.path.abspath(__file__))
    g = GLTF(args.gltf)
    skel = Skeleton(g)
    print(f"{os.path.basename(args.gltf)}: {len(skel.joints)} bones, "
          f"{len(g.j['meshes'][0]['primitives'])} parts, {len(g.j.get('animations', []))} animations")

    model_dir = os.path.dirname(args.name)
    material_path = f"models/{args.name}"
    mat_out = os.path.join(args.out, "materials", material_path)
    os.makedirs(mat_out, exist_ok=True)

    if args.no_textures:
        names = [safe_name(m.get("name"), f"material_{i}") for i, m in enumerate(g.j.get("materials", [{}]))]
    else:
        names = export_materials(g, mat_out, material_path, tools_dir)
    print(f"  materials: {', '.join(names)}")

    # The intermediates go beside the compiler's output and stay there; they are the source a modder would edit.
    work = os.path.join(args.out, "modelsrc", args.name)
    os.makedirs(work, exist_ok=True)
    base = os.path.basename(args.name)

    ref = os.path.join(work, base + "_reference.smd")
    write_reference_smd(ref, g, skel, args.scale, names)

    sequences = []
    for anim in g.j.get("animations", []):
        seq = safe_name(anim.get("name"), f"anim_{len(sequences)}")
        poses = sample_animation(g, skel, anim)
        write_anim_smd(os.path.join(work, seq + ".smd"), skel, poses)
        sequences.append((seq, len(poses)))
        print(f"  {seq}: {len(poses)} frames")

    if not sequences:
        # studiomdl insists on at least one sequence; the bind pose serves.
        write_anim_smd(os.path.join(work, "idle.smd"), skel, [skel.bind_locals()])
        sequences.append(("idle", 1))

    qc_path = os.path.join(work, base + ".qc")
    with open(qc_path, "w", encoding="ascii") as f:
        f.write(f'$modelname "{args.name}.mdl"\n')
        f.write(f'$model "{base}" "{base}_reference.smd"\n')
        f.write(f'$cdmaterials "{material_path}"\n')
        f.write(f'$surfaceprop "{args.surfaceprop}"\n')
        f.write('$staticprop\n' if not sequences else '')
        f.write('$cbox 0 0 0 0 0 0\n')
        f.write('$bbox -16 -16 0 16 16 72\n\n')

        # NOTE: a $definebone for every bone was tried here, to stop studiomdl discarding the bones no vertex is
        # weighted to - it reduces the 65-bone rig to 10 for a legs-only mesh. It does keep them, but it makes
        # the deformation worse rather than better: the animated mesh collapses toward the origin instead of
        # merely distorting, so the six fixup columns it wants are evidently not all zero. Left out until that
        # is understood. Bone culling is not the cause of the animation defect either way - the mesh deforms
        # wrongly with all 65 bones present. See "Known defect" in the README.
        for seq, frames in sequences:
            loop = "" if seq in ("death", "fall") else " loop"
            f.write(f'$sequence "{seq}" "{seq}.smd" fps {FPS:.0f}{loop}\n')

    print(f"  qc: {qc_path}")
    cmd = [studiomdl, "-game", game, "-nop4", "-quiet", qc_path]
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=work)
    tail = (r.stdout + r.stderr).strip().splitlines()
    for line in tail[-12:]:
        print("   " + line)

    # studiomdl writes into the game dir it was given; move the result where it belongs.
    built = os.path.join(game, "models", args.name)
    dest_dir = os.path.join(args.out, "models", model_dir)
    os.makedirs(dest_dir, exist_ok=True)
    moved = 0
    for ext in (".mdl", ".vvd", ".vtx", ".dx90.vtx", ".dx80.vtx", ".sw.vtx", ".phy", ".ani"):
        src = built + ext
        if os.path.isfile(src):
            with open(src, "rb") as a:
                blob = a.read()
            with open(os.path.join(dest_dir, base + ext), "wb") as b:
                b.write(blob)
            os.remove(src)
            moved += 1
    if moved == 0:
        sys.exit("studiomdl produced nothing - see its output above")
    print(f"  -> {dest_dir} ({moved} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
