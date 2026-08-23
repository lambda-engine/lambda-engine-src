"""
Imports Half-Life: Alyx's bullet-impact decals into the mod directory, in the Source 1 formats the runtime reads.

Source 2 ships what Source 1 never had: per-surface bullet holes with authored colour, normal, height and
ambient-occlusion maps, drawn by a parallax-occlusion decal shader (vr_projected_decals: F_PARALLAX,
g_nMinSamples 8, g_nMaxSamples 32, g_flHeightMapScale 0.1). This script pulls those four maps out of the HL:A
VPKs with ValveResourceFormat, writes them as VTFs, and writes a VMT per decal that our material library turns
into the PBR decal master. The group file that picks a decal per surface is HL:A's own scripts/decals_subrect.txt,
merged over HL2's so surfaces HL:A has no decal for keep their HL2 one.

The VMT keys $heightmap, $aotexture, $heightmapscale, $decalsize and $decalsizevariance are a Lambda extension,
not Source 1 keys; $bumpmap is Source 1's own name for a normal map.

Requires: Python 3 with Pillow, the .NET SDK, and a build of ValveResourceFormat's CLI (Source2Viewer-CLI.dll).

Usage:
  python ImportSource2Decals.py --vrf <path/to/Source2Viewer-CLI.dll> ^
      --pak "D:/SteamLibrary/steamapps/common/Half-Life Alyx/game/hlvr/pak01_dir.vpk" ^
      --out "<repo>/game/Game/lambda" ^
      --hl2-decals "C:/Program Files (x86)/Steam/steamapps/common/Source SDK Base 2013 Singleplayer/hl2/scripts/decals_subrect.txt"

The converted textures are Valve's content: they may be used against an owned copy of the game, not redistributed.
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

from PIL import Image

# HL:A decal sizes are in Source 2 units (inches); ours are Hammer units at 1.905 cm. 2.54 / 1.905.
S2_UNITS_TO_HAMMER = 2.54 / 1.905

# VTF image formats (public/bitmap/imageformat.h)
IMAGE_FORMAT_I8 = 5
IMAGE_FORMAT_BGRA8888 = 12
IMAGE_FORMAT_NONE = 0xFFFFFFFF

# VTF flags (public/vtf/vtf.h)
TEXTUREFLAGS_CLAMPS = 0x00000004
TEXTUREFLAGS_CLAMPT = 0x00000008
TEXTUREFLAGS_NORMAL = 0x00000080
TEXTUREFLAGS_EIGHTBITALPHA = 0x00002000

# HL2 game material character -> the HL:A decal group to prefer for it. Anything not listed keeps the group HL2's
# TranslationData already names. HL:A's metal grazing/thin variants are deliberately not used.
PREFERRED_GROUPS = {
    'C': 'Impact.Concrete',
    'M': 'Impact.Metal',
    'W': 'Impact.Wood',
    'Y': 'Impact.Glass',
    'F': 'Impact.Flesh',
    'N': 'Impact.Sand',
    'D': 'Impact.Dirt',
    'T': 'Impact.Tile',
    'V': 'Impact.Vent',
    'P': 'Impact.Computer',
    'G': 'Impact.Metal',
    'L': 'Impact.Plastic',
    'O': 'Impact.Wood',
}


def run_vrf(vrf, args, capture=True):
    cmd = ['dotnet', vrf] + args
    result = subprocess.run(cmd, capture_output=capture, text=True, encoding='utf-8', errors='replace')
    if result.returncode != 0 and capture:
        sys.stderr.write(result.stdout[-2000:] + result.stderr[-2000:])
        raise RuntimeError('VRF failed: ' + ' '.join(args))
    return result.stdout if capture else ''


def parse_kv1_groups(text):
    """Parses a decals_subrect-style KV1 file into an ordered list of (group, [(entry, weight)])."""
    text = re.sub(r'//[^\n]*', '', text)
    tokens = [m.group(1) if m.group(1) is not None else m.group(2)
              for m in re.finditer(r'"([^"]*)"|(\{|\}|[^\s"{}]+)', text)]
    groups = []
    i = 0
    while i < len(tokens):
        if i + 1 < len(tokens) and tokens[i + 1] == '{':
            name = tokens[i]
            i += 2
            entries = []
            while i < len(tokens) and tokens[i] != '}':
                key = tokens[i]
                value = tokens[i + 1] if i + 1 < len(tokens) and tokens[i + 1] not in ('{', '}') else '1'
                entries.append((key, value))
                i += 2
            groups.append((name, entries))
        i += 1
    return groups


def parse_vmat_data(text):
    """Pulls the texture params and the handful of floats we use out of VRF's KV3 text dump of a vmat_c."""
    info = {'textures': {}, 'floats': {}}
    for m in re.finditer(r'm_name = "(g_t\w+)"\s*m_pValue = resource:"([^"]+)"', text):
        info['textures'][m.group(1)] = m.group(2)
    for m in re.finditer(r'm_name = "(\w+)"\s*m_flValue = ([0-9.\-eE]+)', text):
        info['floats'][m.group(1)] = float(m.group(2))
    m = re.search(r'm_name = "PhysicsSurfaceProperties"\s*m_value = "([^"]*)"', text)
    info['surfaceprop'] = m.group(1) if m else ''
    return info


def write_vtf(path, image, fmt, flags):
    """Writes a VTF 7.1 with a full mip chain. image: PIL 'RGBA' (BGRA8888) or 'L' (I8)."""
    w, h = image.size
    mips = [image]
    while w > 1 or h > 1:
        w, h = max(1, w // 2), max(1, h // 2)
        mips.append(mips[-1].resize((w, h), Image.LANCZOS))

    def encode(im):
        if fmt == IMAGE_FORMAT_BGRA8888:
            r, g, b, a = im.convert('RGBA').split()
            return Image.merge('RGBA', (b, g, r, a)).tobytes()
        return im.convert('L').tobytes()

    # Smallest mip first, as the format stores them.
    data = b''.join(encode(m) for m in reversed(mips))

    header = bytearray(64)
    struct.pack_into('<4s', header, 0, b'VTF\0')
    struct.pack_into('<II', header, 4, 7, 1)
    struct.pack_into('<I', header, 12, 64)
    struct.pack_into('<HH', header, 16, image.size[0], image.size[1])
    struct.pack_into('<I', header, 20, flags)
    struct.pack_into('<HH', header, 24, 1, 0)
    struct.pack_into('<fff', header, 32, 0.5, 0.5, 0.5)
    struct.pack_into('<f', header, 48, 1.0)
    struct.pack_into('<I', header, 52, fmt)
    header[56] = len(mips)
    struct.pack_into('<I', header, 57, IMAGE_FORMAT_NONE)
    header[61] = 0
    header[62] = 0

    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(bytes(header))
        f.write(data)


def write_vmt(path, tex, surfaceprop, height_scale, size_units, variance):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8') as f:
        f.write('// Converted from Half-Life: Alyx by Tools/ImportSource2Decals.py. Colour, normal, height and AO maps\n')
        f.write('// are authored; $heightmap, $aotexture, $heightmapscale, $decalsize and $decalsizevariance are a\n')
        f.write('// Lambda extension read by the PBR decal master (Source 2: vr_projected_decals F_PARALLAX).\n')
        f.write('"LightmappedGeneric"\n{\n')
        f.write('\t"$decal"\t\t\t\t1\n\t"$translucent"\t\t\t1\n')
        f.write(f'\t"$basetexture"\t\t\t"{tex["color"]}"\n')
        if 'normal' in tex: f.write(f'\t"$bumpmap"\t\t\t\t"{tex["normal"]}"\n')
        if 'height' in tex: f.write(f'\t"$heightmap"\t\t\t"{tex["height"]}"\n')
        if 'ao' in tex: f.write(f'\t"$aotexture"\t\t\t"{tex["ao"]}"\n')
        f.write(f'\t"$heightmapscale"\t\t{height_scale:g}\n')
        f.write(f'\t"$decalsize"\t\t\t{size_units:.3f}\t// Hammer units; HL:A authored {size_units / S2_UNITS_TO_HAMMER:.2f} inches\n')
        f.write(f'\t"$decalsizevariance"\t{variance:.3f}\t// +/- Hammer units\n')
        if surfaceprop: f.write(f'\t"$surfaceprop"\t\t\t"{surfaceprop}"\n')
        f.write('}\n')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--vrf', required=True, help='path to Source2Viewer-CLI.dll')
    ap.add_argument('--pak', required=True, help="HL:A game/hlvr/pak01_dir.vpk")
    ap.add_argument('--out', required=True, help='mod directory (the one with gameinfo.txt)')
    ap.add_argument('--hl2-decals', required=True, help="HL2's scripts/decals_subrect.txt, the base the HL:A groups are merged over")
    ap.add_argument('--prefix', default='decals/hla', help='material path prefix inside materials/ for the converted decals')
    ap.add_argument('--groups', default='', help='comma-separated HL:A groups to import (default: all)')
    args = ap.parse_args()

    tmp = tempfile.mkdtemp(prefix='hla_decals_')
    print(f'[import] working in {tmp}')

    # 1. HL:A's decal group file.
    run_vrf(args.vrf, ['-i', args.pak, '-f', 'scripts/decals_subrect.txt', '-o', tmp])
    hla_text = open(os.path.join(tmp, 'scripts', 'decals_subrect.txt'), encoding='utf-8', errors='replace').read()
    hla_groups = [(g, e) for g, e in parse_kv1_groups(hla_text) if g.lower() != 'translationdata']
    wanted = set(x.strip().lower() for x in args.groups.split(',') if x.strip())
    if wanted:
        hla_groups = [(g, e) for g, e in hla_groups if g.lower() in wanted]
    vmats = sorted({entry for _, entries in hla_groups for entry, _ in entries})
    print(f'[import] {len(hla_groups)} HL:A decal groups, {len(vmats)} distinct decal materials')

    # 2. Decode every decal texture in one pass (VRF loads the VPK index per invocation, so batch it).
    print('[import] decoding decal textures with VRF...')
    run_vrf(args.vrf, ['-i', args.pak, '-f', 'materials/decals/', '-e', 'vtex_c', '-d', '-o', tmp], capture=True)

    # 3. Each material: read its params, convert its maps, write the VMT.
    converted = {}
    for vmat in vmats:
        vmat_c = vmat + '_c'
        text = run_vrf(args.vrf, ['-i', args.pak, '--vpk_filepath', vmat_c, '-b', 'DATA'])
        info = parse_vmat_data(text)
        textures = info['textures']
        if 'g_tColor' not in textures:
            print(f'  skip {vmat}: no g_tColor')
            continue

        base = os.path.splitext(os.path.basename(vmat))[0]
        sub = os.path.basename(os.path.dirname(vmat))           # "concrete", "metal", ... or "decals" for top-level
        if sub == 'decals':
            sub = re.sub(r'\d+$', '', base)                      # "concrete1" -> "concrete"
        rel = f'{args.prefix}/{sub}/{base}'

        def png_for(vtex):
            return os.path.join(tmp, vtex.replace('.vtex', '.png'))

        tex_out = {}
        jobs = [('color', 'g_tColor', IMAGE_FORMAT_BGRA8888, TEXTUREFLAGS_EIGHTBITALPHA),
                ('normal', 'g_tNormal', IMAGE_FORMAT_BGRA8888, TEXTUREFLAGS_NORMAL),
                ('height', 'g_tHeight', IMAGE_FORMAT_I8, 0),
                ('ao', 'g_tAmbientOcclusion', IMAGE_FORMAT_I8, 0)]
        for key, param, fmt, flags in jobs:
            if param not in textures:
                continue
            png = png_for(textures[param])
            if not os.path.exists(png):
                print(f'  missing decoded texture {png}')
                continue
            img = Image.open(png)
            if fmt == IMAGE_FORMAT_I8:
                img = img.convert('RGBA').getchannel('R')
            else:
                img = img.convert('RGBA')
            name = f'{rel}_{key}'
            write_vtf(os.path.join(args.out, 'materials', name + '.vtf'), img, fmt, flags | TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT)
            tex_out[key] = name

        if 'color' not in tex_out:
            # Blood and fluid decals keep their textures under materials/particle/, outside what we decode; they are
            # not bullet holes and HL2's own stay in use for them.
            print(f'  skip {vmat}: colour texture not decoded')
            continue

        floats = info['floats']
        size_inches = floats.get('DecalWorldWidth', 3.25)
        # DecalSizeVariance is an absolute +/- on the size, in the same inches (0.5 on a 2.0-inch metal hole,
        # 1.25 on a 3.25-inch concrete one), so it converts to Hammer units the same way.
        write_vmt(os.path.join(args.out, 'materials', rel + '.vmt'), tex_out, info['surfaceprop'],
                  floats.get('g_flHeightMapScale', 0.1), size_inches * S2_UNITS_TO_HAMMER,
                  floats.get('DecalSizeVariance', 0.0) * S2_UNITS_TO_HAMMER)
        converted[vmat] = rel
        print(f'  {vmat} -> materials/{rel}.vmt ({", ".join(tex_out)})')

    # 4. Merge the groups over HL2's file: HL:A's group replaces HL2's of the same name, new groups are added,
    #    and TranslationData points each HL2 game material at the best HL:A group available.
    hl2_text = open(args.hl2_decals, encoding='utf-8', errors='replace').read()
    hl2_groups = parse_kv1_groups(hl2_text)
    translation = next((e for g, e in hl2_groups if g.lower() == 'translationdata'), [])
    merged = {g: e for g, e in hl2_groups if g.lower() != 'translationdata'}
    for g, entries in hla_groups:
        new_entries = [(f'{converted[v]}', w) for v, w in entries if v in converted]
        if new_entries:
            merged[g] = new_entries
    group_names_lower = {g.lower(): g for g in merged}
    new_translation = []
    for char, group in translation:
        preferred = PREFERRED_GROUPS.get(char.upper())
        if preferred and preferred.lower() in group_names_lower:
            new_translation.append((char, group_names_lower[preferred.lower()]))
        else:
            new_translation.append((char, group))

    out_path = os.path.join(args.out, 'scripts', 'decals_subrect.txt')
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, 'w', encoding='utf-8') as f:
        f.write('// Generated by Tools/ImportSource2Decals.py: HL2\'s decals_subrect.txt with the bullet-impact groups\n')
        f.write('// replaced by Half-Life: Alyx\'s authored decals (colour/normal/height/AO). Re-run the tool to regenerate.\n\n')
        f.write('"TranslationData"\n{\n')
        for char, group in new_translation:
            f.write(f'\t"{char}"\t\t"{group}"\n')
        f.write('}\n\n')
        for g, entries in merged.items():
            f.write(f'"{g}"\n{{\n')
            for entry, weight in entries:
                f.write(f'\t"{entry}" "{weight}"\n')
            f.write('}\n\n')
    print(f'[import] wrote {out_path} ({len(merged)} groups)')
    shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    main()
