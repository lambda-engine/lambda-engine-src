#!/usr/bin/env python3
"""
Converts one Half-Life: Alyx model material (vr_skin / vr_complex) into a Source 1 VMT plus VTF textures that
Lambda Engine's PBR model master understands.

What is carried over (everything else in the vmat is Source 2 shading we do not have: bent normals, two-lobe
specular, diffuse/shadow warps, transmission):
  g_tColor          -> $basetexture        (BGRA8888, sRGB)
  g_tNormal         -> $bumpmap            (BGRA8888, linear; tangent-space, Source 2 keeps +Y up like UE's -Y
                                            flipped... the VMT carries $normalmapflipy so the master can choose)
  g_tCombinedMasks  -> $selfillummask      (I8 from the channel that carries the self-illum mask, when F_SELF_ILLUM)
  g_vColorTint      -> $color2
  g_flSelfIllumBrightness * g_vSelfIllumTint -> $selfillumtint
  TextureGlossiness / TextureRoughness / g_flMetalness -> $roughness / $metalness (Lambda keys, constants 0..1)
  PhysicsSurfaceProperties -> $surfaceprop (mapped to the Source 1 name when known)

Usage:
  python ImportSource2Material.py --vrf Source2Viewer-CLI.dll --pak "...\\hlvr\\pak01_dir.vpk" --out <moddir>
      --vmat models/creatures/headcrab/materials/headcrab.vmat --material models/hlvr/creatures/headcrab/body
      [--texture-prefix models/hlvr/creatures/headcrab/hla_headcrab] [--max-size 2048] [--translucent]
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ImportSource2Decals import (IMAGE_FORMAT_BGRA8888, IMAGE_FORMAT_I8, TEXTUREFLAGS_EIGHTBITALPHA, TEXTUREFLAGS_NORMAL,  # noqa: E402
                                 run_vrf, write_vtf)

# Source 2 physics surface -> Source 1 $surfaceprop for the ones NPC models use.
SURFACEPROPS = {
    'npc_headcrab_flesh': 'alienflesh',
    'alienflesh': 'alienflesh',
    'flesh': 'flesh',
    'zombieflesh': 'zombieflesh',
    'metal': 'metal',
    'default': 'default',
}


def parse_vmat(text):
    info = {'shader': '', 'textures': {}, 'floats': {}, 'ints': {}, 'vectors': {}, 'strings': {}}
    m = re.search(r'm_shaderName = "([^"]+)"', text)
    info['shader'] = m.group(1) if m else ''
    for m in re.finditer(r'm_name = "(g_t\w+)"\s*m_pValue = resource:"([^"]+)"', text):
        info['textures'][m.group(1)] = m.group(2)
    for m in re.finditer(r'm_name = "(\w+)"\s*m_flValue = ([0-9.\-eE]+)', text):
        info['floats'][m.group(1)] = float(m.group(2))
    for m in re.finditer(r'm_name = "(\w+)"\s*m_nValue = (-?\d+)', text):
        info['ints'][m.group(1)] = int(m.group(2))
    for m in re.finditer(r'm_name = "(\w+)"\s*m_value = \[([^\]]+)\]', text):
        info['vectors'][m.group(1)] = [float(x) for x in m.group(2).split(',')]
    for m in re.finditer(r'm_name = "(\w+)"\s*m_value = "([^"]*)"', text):
        info['strings'][m.group(1)] = m.group(2)
    return info


def decode(vrf, pak, vtex, tmp):
    """Decodes one vtex_c to PNG and returns the PIL image."""
    run_vrf(vrf, ['-i', pak, '-f', vtex + '_c', '-d', '-o', tmp])
    png = os.path.join(tmp, vtex.replace('.vtex', '.png'))
    if not os.path.exists(png):
        raise RuntimeError(f'VRF did not produce {png}')
    return Image.open(png)


def fit(img, max_size):
    if max(img.size) > max_size:
        s = max_size / float(max(img.size))
        img = img.resize((max(1, int(img.size[0] * s)), max(1, int(img.size[1] * s))), Image.LANCZOS)
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--vrf', required=True)
    ap.add_argument('--pak', required=True)
    ap.add_argument('--out', required=True, help='mod directory')
    ap.add_argument('--vmat', required=True, help='vmat path inside the VPK (without _c)')
    ap.add_argument('--material', required=True, help='Source 1 material to write, relative to materials/ ("models/hlvr/creatures/headcrab/body")')
    ap.add_argument('--texture-prefix', default='', help='where the VTFs go, relative to materials/ (default: next to the VMT, named after it)')
    ap.add_argument('--max-size', type=int, default=2048)
    ap.add_argument('--translucent', action='store_true', help='write $translucent 1 (alpha from the colour texture)')
    ap.add_argument('--surfaceprop', default='', help='override $surfaceprop')
    args = ap.parse_args()

    text = run_vrf(args.vrf, ['-i', args.pak, '--vpk_filepath', args.vmat + '_c', '-b', 'DATA'])
    info = parse_vmat(text)
    textures = info['textures']
    if 'g_tColor' not in textures:
        raise SystemExit(f'{args.vmat}: no g_tColor')
    print(f'[import] {args.vmat}: shader {info["shader"]}, textures {", ".join(sorted(textures))}')

    prefix = args.texture_prefix or args.material
    tmp = tempfile.mkdtemp(prefix='hla_vmat_')
    out_materials = os.path.join(args.out, 'materials')

    # colour
    color = fit(decode(args.vrf, args.pak, textures['g_tColor'], tmp).convert('RGBA'), args.max_size)
    write_vtf(os.path.join(out_materials, prefix + '_color.vtf'), color, IMAGE_FORMAT_BGRA8888,
              TEXTUREFLAGS_EIGHTBITALPHA if args.translucent else 0)
    lines = [f'// Converted from Half-Life: Alyx {args.vmat} ({info["shader"]}) by Tools/ImportSource2Material.py.',
             '// $bumpmap/$selfillummask/$color2/$selfillumtint are stock VertexLitGeneric keys; $roughness and $metalness are',
             '// Lambda keys (constants 0..1) read by the PBR model master. Source 2 shading this does not carry over: bent',
             '// normals, two-lobe specular, diffuse/shadow falloff warps, transmission, cube-map specular.',
             '"VertexLitGeneric"', '{',
             f'\t"$basetexture"\t\t"{prefix}_color"']
    if args.translucent:
        lines.append('\t"$translucent"\t\t1')

    # normal (+ roughness in alpha when the texture carries one: HL:A packs it there for vr_complex; vr_skin's is flat)
    if 'g_tNormal' in textures:
        normal = fit(decode(args.vrf, args.pak, textures['g_tNormal'], tmp).convert('RGBA'), args.max_size)
        write_vtf(os.path.join(out_materials, prefix + '_normal.vtf'), normal, IMAGE_FORMAT_BGRA8888,
                  TEXTUREFLAGS_NORMAL | TEXTUREFLAGS_EIGHTBITALPHA)
        lines.append(f'\t"$bumpmap"\t\t\t"{prefix}_normal"')

    # self-illum: g_tCombinedMasks holds it (the channel varies by shader; take the brightest non-constant one)
    if info['ints'].get('F_SELF_ILLUM', 0) and 'g_tCombinedMasks' in textures:
        masks = fit(decode(args.vrf, args.pak, textures['g_tCombinedMasks'], tmp).convert('RGBA'), args.max_size)
        best, best_range = None, -1
        for ch in ('R', 'G', 'B', 'A'):
            band = masks.getchannel(ch)
            lo, hi = band.getextrema()
            if hi - lo > best_range and hi - lo > 2 and lo < 128:
                best, best_range = band, hi - lo
        if best is not None:
            write_vtf(os.path.join(out_materials, prefix + '_selfillum.vtf'), best, IMAGE_FORMAT_I8, 0)
            tint = info['vectors'].get('g_vSelfIllumTint', [1, 1, 1, 0])[:3]
            brightness = info['floats'].get('g_flSelfIllumBrightness', 1.0)
            lines.append('\t"$selfillum"\t\t\t1')
            lines.append(f'\t"$selfillummask"\t\t"{prefix}_selfillum"')
            lines.append('\t"$selfillumtint"\t\t"[%.3f %.3f %.3f]"' % tuple(c * brightness for c in tint))

    tint = info['vectors'].get('g_vColorTint')
    if tint and any(abs(c - 1.0) > 1e-3 for c in tint[:3]):
        lines.append('\t"$color2"\t\t\t"[%.3f %.3f %.3f]"' % tuple(tint[:3]))

    # roughness: vr_complex has TextureRoughness (a constant when no texture); vr_skin has two glossiness lobes
    # mixed by g_flSecondSpecularLobeRatio - collapsed to one roughness for a single-lobe BRDF.
    roughness = None
    if 'TextureRoughness' in info['vectors']:
        roughness = info['vectors']['TextureRoughness'][0]
    elif 'TextureGlossiness' in info['vectors']:
        g = info['vectors']['TextureGlossiness']
        ratio = info['floats'].get('g_flSecondSpecularLobeRatio', 0.0) if info['ints'].get('F_TWO_LOBE_SPECULAR', 0) else 0.0
        gloss = g[0] * (1.0 - ratio) + g[1] * ratio
        roughness = 1.0 - gloss
    if roughness is not None:
        lines.append(f'\t"$roughness"\t\t\t{roughness:.3f}')
    if 'g_flMetalness' in info['floats'] or 'TextureMetalness' in info['vectors']:
        metal = info['floats'].get('g_flMetalness', info['vectors'].get('TextureMetalness', [0])[0])
        lines.append(f'\t"$metalness"\t\t{metal:.3f}')

    surf = args.surfaceprop or SURFACEPROPS.get(info['strings'].get('PhysicsSurfaceProperties', '').lower(), '')
    if surf:
        lines.append(f'\t"$surfaceprop"\t\t"{surf}"')
    lines.append('}')

    vmt_path = os.path.join(out_materials, args.material + '.vmt')
    os.makedirs(os.path.dirname(vmt_path), exist_ok=True)
    with open(vmt_path, 'w', encoding='utf-8', newline='\r\n') as f:
        f.write('\n'.join(lines) + '\n')
    print(f'[import] wrote {vmt_path}')


if __name__ == '__main__':
    main()
