#!/usr/bin/env python3
"""
Converts a Half-Life: Alyx model (vmdl_c) into a Source 1 studio model, compiled with studiomdl.

The pipeline: ValveResourceFormat decompiles the vmdl (a KV3 text file naming bones, hitboxes, attachments,
bodygroups, physics capsules and per-animation events) and exports the same model to glTF (mesh, skin weights,
and every animation baked per bone). This tool reads both, rebuilds SMD reference/animation files plus a QC in
Source 1 terms, and runs studiomdl. A profile JSON picks the meshes and maps HL:A animation names onto Source
activities (see zombie_classic_profile.json).

What carries over from the data: skeleton (pruned to Source 1's 128-bone budget, weights reassigned to the
nearest kept ancestor), meshes/UVs/weights, hitboxes with their hit groups, attachments, bodygroups, animations
(sampled at 30 fps), the animations' own AE_* events, walk/run root motion (moved onto the motion bone so
studiomdl's LX LY extraction finds it), and the ragdoll capsules. What is authored: joint constraint limits (the
compiled Source 2 physics joints are not in the export) and synthesized footstep events (foot-contact frames
measured from the animation).

Usage:
  python ImportSource2Model.py --vrf ... --pak ...\\pak01_dir.vpk
      --vmdl models/creatures/zombie_classic/zombie_classic.vmdl
      --glb <exported .glb> --vmdl-text <decompiled .vmdl> --pak-list <VRF -l output>
      --out <moddir> --name hla/zombie_classic --profile zombie_classic_profile.json
      --studiomdl "C:\\...\\Half-Life 2\\bin\\studiomdl.exe"
"""
import argparse
import json
import math
import os
import re
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ImportSource2Decals import run_vrf  # noqa: E402

FPS = 30.0

# ---------------------------------------------------------------- small matrix/quaternion helpers (rows of 3-lists)

def m_ident():
    return [[1, 0, 0], [0, 1, 0], [0, 0, 1]]

def m_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]

def m_vec(a, v):
    return [sum(a[i][k] * v[k] for k in range(3)) for i in range(3)]

def m_t(a):
    return [[a[j][i] for j in range(3)] for i in range(3)]

def q_to_m(q):
    x, y, z, w = q
    return [[1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
            [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
            [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)]]

def q_slerp(a, b, t):
    d = sum(x * y for x, y in zip(a, b))
    if d < 0:
        b = [-x for x in b]
        d = -d
    if d > 0.9995:
        out = [x + t * (y - x) for x, y in zip(a, b)]
    else:
        th = math.acos(max(-1.0, min(1.0, d)))
        sa = math.sin((1 - t) * th) / math.sin(th)
        sb = math.sin(t * th) / math.sin(th)
        out = [sa * x + sb * y for x, y in zip(a, b)]
    n = math.sqrt(sum(x * x for x in out)) or 1.0
    return [x / n for x in out]

def matrix_to_smd_euler(m):
    """Source's MatrixAngles, returned as SMD's (rotX=roll, rotY=pitch, rotZ=yaw) in radians."""
    fwd = [m[0][0], m[1][0], m[2][0]]
    left = [m[0][1], m[1][1], m[2][1]]
    up = [m[0][2], m[1][2], m[2][2]]
    xy = math.hypot(fwd[0], fwd[1])
    if xy > 0.001:
        yaw = math.atan2(fwd[1], fwd[0])
        pitch = math.atan2(-fwd[2], xy)
        roll = math.atan2(left[2], up[2])
    else:
        yaw = math.atan2(-left[0], left[1])
        pitch = math.atan2(-fwd[2], xy)
        roll = 0.0
    return roll, pitch, yaw

# glTF (y up, metres) -> Source (z up, inches): p_src = P * p / 0.0254 with the cyclic permutation below.
PERM = [[0, 0, 1], [1, 0, 0], [0, 1, 0]]
PERM_T = m_t(PERM)
TO_INCH = 1.0 / 0.0254

def pos_to_source(p):
    return [x * TO_INCH for x in m_vec(PERM, p)]

def rot_to_source(m):
    return m_mul(PERM, m_mul(m, PERM_T))

# ---------------------------------------------------------------- KV3 text parser (the decompiled .vmdl)

class KV3:
    def __init__(self, text):
        # strip the header comment and single-line comments
        text = re.sub(r'<!--.*?-->', '', text, flags=re.S)
        self.t = text
        self.i = 0
        self.n = len(text)

    def ws(self):
        while self.i < self.n:
            c = self.t[self.i]
            if c in ' \t\r\n,':
                self.i += 1
            elif self.t.startswith('//', self.i):
                self.i = self.t.find('\n', self.i)
                if self.i < 0:
                    self.i = self.n
            else:
                break

    def value(self):
        self.ws()
        c = self.t[self.i]
        if c == '{':
            return self.obj()
        if c == '[':
            return self.arr()
        if c == '"':
            return self.string()
        if self.t.startswith('resource:', self.i):
            self.i += len('resource:')
            return self.string()
        m = re.match(r'[-+0-9.eE]+', self.t[self.i:])
        if m and m.group(0) and any(ch.isdigit() for ch in m.group(0)):
            self.i += m.end()
            s = m.group(0)
            return float(s) if ('.' in s or 'e' in s or 'E' in s) else int(s)
        m = re.match(r'[A-Za-z_][A-Za-z0-9_]*', self.t[self.i:])
        if m:
            self.i += m.end()
            w = m.group(0)
            return {'true': True, 'false': False, 'null': None}.get(w, w)
        raise ValueError(f'KV3 parse error at {self.i}: {self.t[self.i:self.i+40]!r}')

    def string(self):
        assert self.t[self.i] == '"'
        j = self.t.index('"', self.i + 1)
        s = self.t[self.i + 1:j]
        self.i = j + 1
        return s

    def obj(self):
        assert self.t[self.i] == '{'
        self.i += 1
        out = {}
        while True:
            self.ws()
            if self.t[self.i] == '}':
                self.i += 1
                return out
            m = re.match(r'[A-Za-z_][A-Za-z0-9_.]*', self.t[self.i:])
            key = m.group(0)
            self.i += m.end()
            self.ws()
            assert self.t[self.i] == '='
            self.i += 1
            out[key] = self.value()

    def arr(self):
        assert self.t[self.i] == '['
        self.i += 1
        out = []
        while True:
            self.ws()
            if self.t[self.i] == ']':
                self.i += 1
                return out
            out.append(self.value())


def walk_kv3(node, cls, out):
    if isinstance(node, dict):
        if node.get('_class') == cls:
            out.append(node)
        for v in node.values():
            walk_kv3(v, cls, out)
    elif isinstance(node, list):
        for v in node:
            walk_kv3(v, cls, out)
    return out

# ---------------------------------------------------------------- glTF reader

class Glb:
    def __init__(self, path):
        b = open(path, 'rb').read()
        jlen = struct.unpack_from('<I', b, 12)[0]
        self.j = json.loads(b[20:20 + jlen])
        binlen, bintype = struct.unpack_from('<II', b, 20 + jlen)
        assert bintype == 0x004E4942
        self.bin = b[20 + jlen + 8:20 + jlen + 8 + binlen]

    def acc(self, i):
        a = self.j['accessors'][i]
        bv = self.j['bufferViews'][a['bufferView']]
        off = bv.get('byteOffset', 0) + a.get('byteOffset', 0)
        ncomp = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}[a['type']]
        fmt = {5126: 'f', 5123: 'H', 5121: 'B', 5125: 'I', 5122: 'h', 5120: 'b'}[a['componentType']]
        sz = struct.calcsize(fmt)
        stride = bv.get('byteStride', ncomp * sz)
        out = []
        norm = a.get('normalized', False)
        maxv = {'H': 65535.0, 'B': 255.0, 'h': 32767.0, 'b': 127.0}.get(fmt, 1.0)
        for k in range(a['count']):
            v = struct.unpack_from('<' + fmt * ncomp, self.bin, off + k * stride)
            if norm and fmt != 'f':
                v = tuple(x / maxv for x in v)
            out.append(v)
        return out


def sample_channel(times, values, t, is_quat):
    if len(times) == 1:
        return list(values[0])
    if t <= times[0]:
        return list(values[0])
    if t >= times[-1]:
        return list(values[-1])
    lo, hi = 0, len(times) - 1
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if times[mid] <= t:
            lo = mid
        else:
            hi = mid
    span = times[hi] - times[lo]
    f = (t - times[lo]) / span if span > 0 else 0.0
    if is_quat:
        return q_slerp(list(values[lo]), list(values[hi]), f)
    return [a + (b - a) * f for a, b in zip(values[lo], values[hi])]

# ---------------------------------------------------------------- main conversion

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--vrf', required=True)
    ap.add_argument('--pak', required=True)
    ap.add_argument('--vmdl', required=True, help='vmdl path inside the VPK (no _c)')
    ap.add_argument('--glb', required=True, help='glb exported by VRF (with animations)')
    ap.add_argument('--vmdl-text', required=True, help='decompiled .vmdl KV3 text file')
    ap.add_argument('--pak-list', required=True, help='VRF -l output (to find each material vmat)')
    ap.add_argument('--out', required=True, help='mod directory')
    ap.add_argument('--name', required=True, help='Source 1 model name, e.g. hla/zombie_classic')
    ap.add_argument('--profile', required=True)
    ap.add_argument('--studiomdl', required=True)
    ap.add_argument('--workdir', default='')
    ap.add_argument('--max-bones', type=int, default=128)
    ap.add_argument('--skip-materials', action='store_true')
    args = ap.parse_args()

    profile = json.load(open(args.profile, encoding='utf-8'))
    work = args.workdir or tempfile.mkdtemp(prefix='s2mdl_')
    os.makedirs(work, exist_ok=True)
    print(f'[model] workdir {work}')

    kv = KV3(open(args.vmdl_text, encoding='utf-8', errors='replace').value_text if False else open(args.vmdl_text, encoding='utf-8', errors='replace').read()).value()
    glb = Glb(args.glb)
    j = glb.j
    nodes = j['nodes']
    parent = {}
    for i, n in enumerate(nodes):
        for c in n.get('children', []):
            parent[c] = i
    node_name = [n.get('name', f'node{i}') for i, n in enumerate(nodes)]
    name_node = {}
    for i, nm in enumerate(node_name):
        name_node.setdefault(nm, i)

    skins = j.get('skins', [])
    joints = skins[0]['joints']
    joint_nodes = list(joints)
    bone_names = [node_name[x] for x in joint_nodes]
    node_is_bone = set(joint_nodes)
    print(f'[model] {len(bone_names)} bones in the skin')

    # ---- vmdl data
    hitbox_sets = walk_kv3(kv, 'HitboxSet', [])
    hitboxes = []
    for hs in hitbox_sets:
        if hs.get('name') == profile.get('hitboxset', 'default'):
            hitboxes = walk_kv3(hs, 'Hitbox', [])
            break
    attachments = [a for a in walk_kv3(kv, 'Attachment', []) if a.get('name') in set(profile.get('attachments', []))]
    capsules = walk_kv3(kv, 'PhysicsShapeCapsule', [])
    animfiles = {a['name']: a for a in walk_kv3(kv, 'AnimFile', [])}
    print(f'[model] vmdl: {len(hitboxes)} hitboxes, {len(attachments)} attachments, {len(capsules)} capsules, {len(animfiles)} animfiles')

    # ---- choose meshes
    wanted_meshes = list(profile.get('meshes', []))
    for bg in profile.get('bodygroups', []):
        for choice in bg['choices']:
            wanted_meshes += choice
    mesh_nodes = {}
    for i, n in enumerate(nodes):
        if 'mesh' in n:
            mesh_name = j['meshes'][n['mesh']].get('name', '').split('.')[-1]
            mesh_nodes[mesh_name] = i
    missing = [m for m in wanted_meshes if m not in mesh_nodes]
    if missing:
        raise SystemExit(f'meshes not in glb: {missing}; available: {sorted(mesh_nodes)[:60]}')

    # ---- bone keep/prune
    weighted = set()
    for mname in set(wanted_meshes):
        ni = mesh_nodes[mname]
        skin = skins[nodes[ni]['skin']]
        for prim in j['meshes'][nodes[ni]['mesh']]['primitives']:
            jo = glb.acc(prim['attributes']['JOINTS_0'])
            we = glb.acc(prim['attributes']['WEIGHTS_0'])
            for jj, ww in zip(jo, we):
                for k in range(4):
                    if ww[k] > 0.001:
                        weighted.add(skin['joints'][jj[k]])
    forced = set()
    for hb in hitboxes:
        if hb['parent_bone'] in name_node:
            forced.add(name_node[hb['parent_bone']])
    for at in attachments:
        if at['parent_bone'] in name_node:
            forced.add(name_node[at['parent_bone']])
    for cp in capsules:
        if cp['parent_bone'] in name_node:
            forced.add(name_node[cp['parent_bone']])
    # the Source 2 motion bone is dropped: the pelvis carries the same translation and, as bone 0 of the
    # compiled model, is exactly what studiomdl's LX LY extraction reads

    prune_patterns = profile.get('prune_patterns', [])
    def prunable(name):
        return any(p in name for p in prune_patterns)

    keep = set()
    for ni in weighted | forced:
        if ni in forced or not prunable(node_name[ni]):
            keep.add(ni)
    # ancestors of kept bones stay
    changed = True
    while changed:
        changed = False
        for ni in list(keep):
            p = parent.get(ni)
            if p is not None and p in node_is_bone and p not in keep:
                keep.add(p)
                changed = True
    kept = [ni for ni in joint_nodes if ni in keep]  # skin order: parents come before children
    if len(kept) > args.max_bones:
        raise SystemExit(f'{len(kept)} bones after pruning (> {args.max_bones}); extend prune_patterns. Kept: {[node_name[k] for k in kept]}')
    bone_index = {ni: i for i, ni in enumerate(kept)}
    def kept_ancestor(ni):
        p = ni
        while p is not None and p not in bone_index:
            p = parent.get(p)
        return p
    smd_parent = []
    for ni in kept:
        p = kept_ancestor(parent.get(ni))
        smd_parent.append(bone_index[p] if p is not None else -1)
    print(f'[model] kept {len(kept)} bones (pruned {len(bone_names) - len(kept)})')

    # ---- local/world transforms
    def node_local(ni, anim_state=None):
        n = nodes[ni]
        t = list(n.get('translation', [0, 0, 0]))
        r = list(n.get('rotation', [0, 0, 0, 1]))
        if anim_state and ni in anim_state:
            st = anim_state[ni]
            t = st.get('translation', t)
            r = st.get('rotation', r)
        return t, r

    def world_transforms(anim_state=None):
        world = {}
        def rec(ni, pm, pt):
            t, r = node_local(ni, anim_state)
            rm = q_to_m(r)
            wm = m_mul(pm, rm)
            wt = [pt[k] + sum(pm[k][x] * t[x] for x in range(3)) for k in range(3)]
            world[ni] = (wm, wt)
            for c in nodes[ni].get('children', []):
                rec(c, wm, wt)
        for root in j['scenes'][0]['nodes']:
            rec(root, m_ident(), [0, 0, 0])
        return world

    def smd_bone_lines(world):
        lines = []
        for i, ni in enumerate(kept):
            wm, wt = world[ni]
            p = kept[smd_parent[i]] if smd_parent[i] >= 0 else None
            if p is None:
                lm, lt = wm, wt
            else:
                pm, pt = world[p]
                pmi = m_t(pm)  # rotation inverse
                lm = m_mul(pmi, wm)
                lt = m_vec(pmi, [wt[k] - pt[k] for k in range(3)])
            sm = rot_to_source(lm)
            st = pos_to_source(lt)
            rx, ry, rz = matrix_to_smd_euler(sm)
            lines.append(f'{i} {st[0]:.6f} {st[1]:.6f} {st[2]:.6f} {rx:.6f} {ry:.6f} {rz:.6f}')
        return lines

    def nodes_block():
        out = ['version 1', 'nodes']
        for i, ni in enumerate(kept):
            out.append(f'{i} "{node_name[ni]}" {smd_parent[i]}')
        out.append('end')
        return out

    bind_world = world_transforms()

    # ---- reference SMDs (one per mesh)
    mat_used = set()
    # How far the skinned mesh reaches past each bone, in Source units: studiomdl derives $bbox from bone
    # positions alone, which leaves whatever hangs off them (a headcrab riding a zombie's head) outside the hull.
    bone_radius = [0.0] * len(kept)
    # Which bones the hull has to bound. Source sizes an NPC's hull to its body, not to where its arms swing
    # (limbs clipping a wall is normal in HL2); "hull_bones" names the ones that must stay inside it.
    hull_bone_names = profile.get('hull_bones', [])
    hull_bone_indices = [i for i, ni in enumerate(kept) if node_name[ni] in hull_bone_names]
    if hull_bone_names and not hull_bone_indices:
        print(f'[model] WARNING: none of hull_bones {hull_bone_names} are kept bones')

    def write_reference(mesh_name, path):
        # "mesh_offsets" nudges a whole mesh in the bind pose (Source units, z up). The headcrab riding a zombie's
        # head is skinned to head bones, so an offset here rides along with it.
        mesh_offset = profile.get('mesh_offsets', {}).get(mesh_name)
        ni = mesh_nodes[mesh_name]
        skin = skins[nodes[ni]['skin']]
        lines = nodes_block() + ['skeleton', 'time 0'] + smd_bone_lines(bind_world) + ['end', 'triangles']
        tri = 0
        for prim in j['meshes'][nodes[ni]['mesh']]['primitives']:
            mat = j['materials'][prim['material']].get('name', 'default') if 'material' in prim else 'default'
            mat_used.add(mat)
            pos = glb.acc(prim['attributes']['POSITION'])
            nrm = glb.acc(prim['attributes']['NORMAL'])
            uv = glb.acc(prim['attributes']['TEXCOORD_0'])
            jo = glb.acc(prim['attributes']['JOINTS_0'])
            we = glb.acc(prim['attributes']['WEIGHTS_0'])
            idx = [x[0] for x in glb.acc(prim['indices'])]
            def vline(v):
                p = pos_to_source(list(pos[v]))
                if mesh_offset:
                    p = [p[k] + mesh_offset[k] for k in range(3)]
                nn = m_vec(PERM, list(nrm[v]))
                u, vv = uv[v]  # SMD's V axis points up; studiomdl flips it back to the top-left origin
                wmap = {}
                for k in range(4):
                    if we[v][k] > 0.001:
                        node_i = skin['joints'][jo[v][k]]
                        ka = kept_ancestor(node_i)
                        if ka is not None:
                            bi = bone_index[ka]
                            wmap[bi] = wmap.get(bi, 0.0) + we[v][k]
                if not wmap:
                    # cloth-simulated vertices carry no skin weights in the export; pin them to the nearest bone
                    p_glb = list(pos[v])
                    best, bestd = 0, 1e30
                    for bi2, ni2 in enumerate(kept):
                        wt2 = bind_world[ni2][1]
                        d = sum((wt2[k2] - p_glb[k2]) ** 2 for k2 in range(3))
                        if d < bestd:
                            bestd, best = d, bi2
                    wmap = {best: 1.0}
                pairs = sorted(wmap.items(), key=lambda kvp: -kvp[1])[:3]
                total = sum(w for _, w in pairs) or 1.0
                ws = ' '.join(f'{b} {w / total:.6f}' for b, w in pairs)
                for bi3, _w3 in pairs:
                    bp = pos_to_source(bind_world[kept[bi3]][1])
                    d = math.dist(p, bp)
                    if d > bone_radius[bi3]:
                        bone_radius[bi3] = d
                return f'0 {p[0]:.4f} {p[1]:.4f} {p[2]:.4f} {nn[0]:.4f} {nn[1]:.4f} {nn[2]:.4f} {u:.6f} {1.0 - vv:.6f} {len(pairs)} {ws}'
            for k in range(0, len(idx), 3):
                # the axis change is a cyclic permutation (a rotation, det +1): the winding is preserved
                a, b, c = idx[k], idx[k + 1], idx[k + 2]
                lines.append(mat)
                lines.append(vline(a))
                lines.append(vline(b))
                lines.append(vline(c))
                tri += 1
        lines.append('end')
        open(path, 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
        print(f'[model]   {os.path.basename(path)}: {tri} tris')

    # ---- animation SMDs
    def anim_channels(anim):
        per_node = {}
        for ch in anim['channels']:
            tgt = ch['target']
            if tgt['path'] not in ('translation', 'rotation'):
                continue
            s = anim['samplers'][ch['sampler']]
            times = [x[0] for x in glb.acc(s['input'])]
            values = glb.acc(s['output'])
            per_node.setdefault(tgt['node'], {})[tgt['path']] = (times, values)
        return per_node

    def write_animation(seq, path):
        anim = next((a for a in j.get('animations', []) if a.get('name') == seq['anim']), None)
        if anim is None:
            raise SystemExit(f'animation {seq["anim"]} not in glb')
        chans = anim_channels(anim)
        maxt = 0.0
        for per in chans.values():
            for times, _ in per.values():
                maxt = max(maxt, times[-1])
        frames = max(2, int(round(maxt * FPS)) + 1)
        motion = bool(seq.get('motion'))

        lines = nodes_block() + ['skeleton']
        foot_heights = {'ankle_L': [], 'ankle_R': []}
        # The hull bounds the model around its origin, so the travel that studiomdl extracts as movement (LX LY)
        # is taken back out here, and the animations that throw the body across the floor - the deaths and the
        # ragdoll - are left out of it entirely.
        reach = model_extents if not seq.get('activity', '').startswith(('ACT_DIE', 'ACT_DIERAGDOLL')) else None
        travel0 = None
        base_root = None
        for f in range(frames):
            t = min(maxt, f / FPS)
            state = {}
            for ni, per in chans.items():
                st = {}
                if 'translation' in per:
                    st['translation'] = sample_channel(per['translation'][0], per['translation'][1], t, False)
                if 'rotation' in per:
                    st['rotation'] = sample_channel(per['rotation'][0], per['rotation'][1], t, True)
                state[ni] = st
            world = world_transforms(state)
            for foot in foot_heights:
                if foot in name_node and name_node[foot] in world:
                    foot_heights[foot].append(world[name_node[foot]][1][1])  # glTF y = height
                else:
                    foot_heights[foot].append(0.0)
            if reach is not None and hull_bone_indices:
                root = pos_to_source(world[kept[0]][1])
                if travel0 is None:
                    travel0 = [root[0], root[1]]
                # Always measured around the model's own root: how far a lunge or a stride carries it is not part
                # of its shape (and studiomdl takes the locomotion out as movement anyway).
                travel = [root[0] - travel0[0], root[1] - travel0[1], 0.0]
                for bi4 in hull_bone_indices:
                    ni4 = kept[bi4]
                    bp = pos_to_source(world[ni4][1])
                    bp = [bp[k] - travel[k] for k in range(3)]
                    r = bone_radius[bi4]
                    reach[0] = min(reach[0], bp[0] - r); reach[3] = max(reach[3], bp[0] + r)
                    reach[1] = min(reach[1], bp[1] - r); reach[4] = max(reach[4], bp[1] + r)
                    reach[2] = min(reach[2], bp[2] - r); reach[5] = max(reach[5], bp[2] + r)
            lines.append(f'time {f}')
            lines += smd_bone_lines(world)
        lines.append('end')
        open(path, 'w', encoding='utf-8').write('\n'.join(lines) + '\n')

        # synthesized footsteps: frames where a foot's height bottoms out
        steps = []
        if seq.get('footsteps'):
            for foot, ev in (('ankle_L', 'AE_ZOMBIE_STEP_LEFT'), ('ankle_R', 'AE_ZOMBIE_STEP_RIGHT')):
                h = foot_heights[foot]
                floor = min(h) + 0.02
                down = False
                for f in range(1, frames):
                    if h[f] <= floor and not down:
                        steps.append((f, ev))
                        down = True
                    elif h[f] > floor + 0.02:
                        down = False
        return frames, steps

    # ---- physics SMD (capsule hulls per bone)
    def write_physics(path):
        lines = nodes_block() + ['skeleton', 'time 0'] + smd_bone_lines(bind_world) + ['end', 'triangles']
        seg = 8
        for cp in capsules:
            bname = cp['parent_bone']
            ka = kept_ancestor(name_node.get(bname))
            if ka is None:
                continue
            bi = bone_index[ka]
            wm, wt = bind_world[name_node[bname]]
            r = cp['radius']
            p0 = cp['point0']
            p1 = cp['point1']
            # capsule points are in the Source 2 bone frame (inches). Build points around the axis, then take
            # them to model space through the glTF bone world transform (glTF frame = P^T * source frame / inch).
            axis = [p1[k] - p0[k] for k in range(3)]
            al = math.sqrt(sum(x * x for x in axis)) or 1.0
            ax = [x / al for x in axis]
            up = [0, 0, 1] if abs(ax[2]) < 0.9 else [1, 0, 0]
            side = [ax[1] * up[2] - ax[2] * up[1], ax[2] * up[0] - ax[0] * up[2], ax[0] * up[1] - ax[1] * up[0]]
            sl = math.sqrt(sum(x * x for x in side)) or 1.0
            side = [x / sl for x in side]
            side2 = [ax[1] * side[2] - ax[2] * side[1], ax[2] * side[0] - ax[0] * side[2], ax[0] * side[1] - ax[1] * side[0]]
            ring0, ring1 = [], []
            for s in range(seg):
                a = 2 * math.pi * s / seg
                off = [r * (math.cos(a) * side[k] + math.sin(a) * side2[k]) for k in range(3)]
                ring0.append([p0[k] + off[k] - ax[k] * r for k in range(3)])
                ring1.append([p1[k] + off[k] + ax[k] * r for k in range(3)])
            def to_model(p_bone):
                # bone-local Source inches -> glTF bone local -> glTF model -> Source model
                pg = [x * 0.0254 for x in m_vec(PERM_T, p_bone)]
                wp = [wt[k] + sum(wm[k][x] * pg[x] for x in range(3)) for k in range(3)]
                return pos_to_source(wp)
            pts0 = [to_model(p) for p in ring0]
            pts1 = [to_model(p) for p in ring1]
            def tri(a, b, c):
                lines.append('phy')
                for p in (a, b, c):
                    lines.append(f'0 {p[0]:.4f} {p[1]:.4f} {p[2]:.4f} 0 0 1 0 0 1 {bi} 1.0')
            for s in range(seg):
                s2 = (s + 1) % seg
                tri(pts0[s], pts1[s], pts1[s2])
                tri(pts0[s], pts1[s2], pts0[s2])
            c0 = to_model([p0[k] - ax[k] * r * 1.2 for k in range(3)])
            c1 = to_model([p1[k] + ax[k] * r * 1.2 for k in range(3)])
            for s in range(seg):
                s2 = (s + 1) % seg
                tri(c0, pts0[s], pts0[s2])
                tri(c1, pts1[s2], pts1[s])
        lines.append('end')
        open(path, 'w', encoding='utf-8').write('\n'.join(lines) + '\n')

    # ---- write everything
    os.makedirs(work, exist_ok=True)
    model_extents = [1e9, 1e9, 1e9, -1e9, -1e9, -1e9]
    ref_files = {}
    for mesh in dict.fromkeys(wanted_meshes):
        fn = f'{mesh}.smd'
        write_reference(mesh, os.path.join(work, fn))
        ref_files[mesh] = fn
    write_physics(os.path.join(work, 'physics.smd'))

    qc = []
    qc.append(f'$modelname "{args.name}.mdl"')
    qc.append(f'$cdmaterials "models/{args.name}/"')
    qc.append(f'$surfaceprop "{profile.get("surfaceprop", "flesh")}"')
    ep = profile.get('eyeposition')
    if ep:
        qc.append(f'$eyeposition {ep[0]} {ep[1]} {ep[2]}')
    for mesh in profile.get('meshes', []):
        qc.append(f'$model "{mesh}" "{ref_files[mesh]}"')
    for bg in profile.get('bodygroups', []):
        qc.append(f'$bodygroup "{bg["name"]}"')
        qc.append('{')
        for choice in bg['choices']:
            if not choice:
                qc.append('\tblank')
            else:
                qc.append(f'\tstudio "{ref_files[choice[0]]}"')
        qc.append('}')
    for at in attachments:
        o = at.get('relative_origin', [0, 0, 0])
        a = at.get('relative_angles', [0, 0, 0])
        qc.append(f'$attachment "{at["name"]}" "{at["parent_bone"]}" {o[0]:.3f} {o[1]:.3f} {o[2]:.3f} rotate {a[0]:.2f} {a[1]:.2f} {a[2]:.2f}')
    qc.append('$hboxset "default"')
    for hb in hitboxes:
        mn = hb['hitbox_mins']
        mx = hb['hitbox_maxs']
        qc.append(f'$hbox {hb.get("group_id", 0)} "{hb["parent_bone"]}" {mn[0]:.2f} {mn[1]:.2f} {mn[2]:.2f} {mx[0]:.2f} {mx[1]:.2f} {mx[2]:.2f}')

    for seq in profile['sequences']:
        fn = f'anims/{seq["name"]}.smd'
        os.makedirs(os.path.join(work, 'anims'), exist_ok=True)
        frames, steps = write_animation(seq, os.path.join(work, fn))
        # studiomdl rotates animation SMDs by +90 degrees of yaw (SMD animations are authored facing +Y, models
        # face +X); the reference SMD is left alone. Measured on this model: every animated bone came out yawed
        # +90 against the file. "rotate -90" takes it back.
        opts = [f'"{fn}"', f'activity {seq["activity"]} {seq.get("weight", 1)}', f'fps {int(FPS)}', 'rotate -90']
        if seq.get('loop'):
            opts.append('loop')
        if seq.get('motion'):
            opts.append('LX LY')
        events = []
        af = animfiles.get(seq['anim'])
        if af:
            for ev in walk_kv3(af, 'AnimEvent', []):
                cls = ev.get('event_class', '')
                if cls.startswith('AE_ZOMBIE_') or cls == 'AE_NPC_ATTACK_BROADCAST':
                    events.append((int(ev.get('event_frame', 0)), cls))
        events += steps
        body = ' '.join(opts)
        qc.append(f'$sequence "{seq["name"]}" ' + body + ' {')
        for frame, cls in sorted(events):
            qc.append(f'\t{{ event {cls} {min(frame, frames - 1)} }}')
        qc.append('}')

    # $bbox last: the extents are only known once every sequence has been written. studiomdl would otherwise
    # derive the hull from bone positions alone, leaving whatever is skinned to them (a headcrab riding a zombie's
    # head) outside the hull - and outside the NPC hull the engine builds from it.
    bbox = profile.get('bbox')
    if not bbox and model_extents[3] > model_extents[0]:
        bbox = [round(model_extents[0], 1), round(model_extents[1], 1), min(0.0, round(model_extents[2], 1)),
                round(model_extents[3], 1), round(model_extents[4], 1), round(model_extents[5], 1)]
        print(f'[model] bbox from the animated mesh: {bbox}')
    if bbox:
        qc.append('$bbox %g %g %g %g %g %g' % tuple(bbox))

    qc.append(f'$collisionjoints "physics.smd"')
    qc.append('{')
    qc.append(f'\t$mass {profile.get("mass", 100)}')
    qc.append('\t$rootbone "pelvis"')
    for bone, lim in profile.get('joint_constraints', {}).items():
        if bone == 'comment' or not isinstance(lim, list):
            continue
        qc.append(f'\t$jointconstrain "{bone}" x limit {lim[0]:.1f} {lim[1]:.1f} 0')
        qc.append(f'\t$jointconstrain "{bone}" y limit {lim[2]:.1f} {lim[3]:.1f} 0')
        qc.append(f'\t$jointconstrain "{bone}" z limit {lim[4]:.1f} {lim[5]:.1f} 0')
    qc.append('}')

    qc_path = os.path.join(work, 'model.qc')
    open(qc_path, 'w', encoding='utf-8').write('\n'.join(qc) + '\n')
    print(f'[model] wrote {qc_path}')

    # ---- materials
    if not args.skip_materials:
        listing = open(args.pak_list, encoding='utf-8', errors='replace').read().splitlines()
        for mat in sorted(mat_used):
            hit = next((l.split(' CRC')[0] for l in listing if l.split(' CRC')[0].endswith(f'/{mat}.vmat_c')), None)
            if not hit:
                print(f'[model] WARNING: no vmat found for material {mat}')
                continue
            cmd = [sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'ImportSource2Material.py'),
                   '--vrf', args.vrf, '--pak', args.pak, '--out', args.out,
                   '--vmat', hit[:-2], '--material', f'models/{args.name}/{mat}']
            print('[model] material', mat)
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode != 0:
                print(r.stdout[-800:], r.stderr[-800:])

    # ---- compile
    gamedir = os.path.abspath(args.out)
    cmd = [args.studiomdl, '-game', gamedir, '-nop4', '-nox360', qc_path]
    print('[model] studiomdl:', ' '.join(cmd))
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=work)
    tail = (r.stdout or '')[-3000:] + (r.stderr or '')[-800:]
    print(tail)
    out_mdl = os.path.join(gamedir, 'models', args.name + '.mdl')
    if os.path.exists(out_mdl):
        print(f'[model] OK -> {out_mdl}')
    else:
        raise SystemExit('[model] studiomdl produced no mdl')


if __name__ == '__main__':
    main()
