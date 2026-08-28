"""
Puts a converted model onto Half-Life's skeleton, so it can wear Half-Life's animations.

ImportGLTFModel.py brings a character in with whatever rig its author used - for anything off Mixamo that is
"mixamorig:LeftArm" and friends. The mesh is fine; the names are not. Every human animation this engine has is
authored against ValveBiped, and a sequence finds its bones by name, so a model whose bones are called something
else can only ever play the animations that shipped inside it - which for a character ripped out of a GLB is
usually a handful of loops, or none.

This renames the rig in place, in the SMD sources the converter already wrote, and recompiles with an
$includemodel pointing at the animation library. Nothing about the mesh changes: the vertices, their weights and
the bind pose are byte for byte what they were, because it is the same reference SMD going back through the same
compiler. Only the labels on the bones are different, and afterwards the model is eligible for the whole
citizen animation set - walks, crouches, jumps, weapon holds, the gestures.

What renaming cannot fix is that the two rigs, agreeing on names, still disagree on where the bones point and
even on how many there are (ValveBiped hangs the clavicles off a Spine4 that Mixamo has no equivalent for).
That part is the engine's: it compares the two bind poses when it merges the library and re-expresses the
borrowed pose through this skeleton's own hierarchy. See FIncludeGroup in SourceMDLFile.h.

    python Tools/ReskeletonToValveBiped.py player/gordon --out ../lambda-engine/Mods/lambda \
        --includemodel models/humans/male_shared.mdl

The model source has to still be on disk, under <out>/modelsrc/<name>/ - which is where the converter leaves it.
"""

import argparse
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ImportGLTFModel import valvebiped_name, RENAME_TO_VALVEBIPED


NODE_LINE = re.compile(r'^(\s*\d+\s+)"([^"]*)"(\s+-?\d+\s*)$')
DEFINEBONE = re.compile(r'^(\$definebone\s+)"([^"]*)"(\s+)"([^"]*)"(.*)$', re.IGNORECASE)


def rename_smd(path):
    """Renames the bones in an SMD's node block. Everything else - skeleton, triangles - is left alone."""
    with open(path, encoding="ascii") as f:
        lines = f.readlines()

    out, in_nodes, renamed = [], False, 0
    for line in lines:
        stripped = line.strip()
        if stripped == "nodes":
            in_nodes = True
        elif in_nodes and stripped == "end":
            in_nodes = False
        elif in_nodes:
            m = NODE_LINE.match(line.rstrip("\n"))
            if m:
                new = valvebiped_name(m.group(2))
                renamed += new != m.group(2)
                line = f'{m.group(1)}"{new}"{m.group(3)}\n'
        out.append(line)

    with open(path, "w", encoding="ascii") as f:
        f.writelines(out)
    return renamed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name", help="model path without extension, as given to the converter, e.g. player/gordon")
    ap.add_argument("--out", required=True, help="mod directory (the one holding models/ and modelsrc/)")
    # The four a citizen pulls in, and the reason to do any of this: male_shared is the locomotion, gestures
    # the attacks and reloads, postures the aim. Named without the "models/" studiomdl prepends for itself.
    ap.add_argument("--includemodel", action="append",
                    help="animation library to borrow sequences from (default: the citizen's four)")
    ap.add_argument("--keep-anims", action="store_true",
                    help="keep the sequences the model was imported with, instead of only the borrowed set")
    ap.add_argument("--studiomdl", help="studiomdl.exe (default: Source SDK Base 2013 Singleplayer)")
    ap.add_argument("--game", help="game dir for studiomdl (default: the SDK's hl2)")
    args = ap.parse_args()

    includes = args.includemodel or [
        "humans/male_shared.mdl", "humans/male_ss.mdl",
        "humans/male_gestures.mdl", "humans/male_postures.mdl",
    ]

    RENAME_TO_VALVEBIPED[0] = True
    sdk = "C:/Program Files (x86)/Steam/steamapps/common/Source SDK Base 2013 Singleplayer"
    studiomdl = args.studiomdl or os.path.join(sdk, "bin", "studiomdl.exe")
    game = args.game or os.path.join(sdk, "hl2")

    base = os.path.basename(args.name)
    model_dir = os.path.dirname(args.name)
    work = os.path.abspath(os.path.join(args.out, "modelsrc", args.name))
    qc_path = os.path.join(work, base + ".qc")
    if not os.path.isfile(qc_path):
        sys.exit(f"no model source at {work} - run ImportGLTFModel.py first, it leaves the SMDs behind")

    # ---------------------------------------------------------------------------------------- the SMDs
    total = 0
    for entry in sorted(os.listdir(work)):
        if entry.lower().endswith(".smd"):
            n = rename_smd(os.path.join(work, entry))
            total += n
            print(f"  {entry}: {n} bones renamed")

    # A single-frame sequence, because studiomdl will not compile a model without one. The reference SMD's own
    # skeleton block is the bind pose, so the file is just its header with the triangles left off.
    with open(os.path.join(work, base + "_reference.smd"), encoding="ascii") as f:
        ref = f.readlines()
    cut = next(i for i, l in enumerate(ref) if l.strip() == "triangles")
    with open(os.path.join(work, "bindpose.smd"), "w", encoding="ascii") as f:
        f.writelines(ref[:cut])

    # ---------------------------------------------------------------------------------------- the QC
    with open(qc_path, encoding="ascii") as f:
        qc = f.readlines()

    out, seen_seq = [], False
    for line in qc:
        m = DEFINEBONE.match(line.strip())
        if m:
            parent = valvebiped_name(m.group(4)) if m.group(4) else ""
            out.append(f'{m.group(1)}"{valvebiped_name(m.group(2))}"{m.group(3)}"{parent}"{m.group(5)}\n')
            continue
        if line.lstrip().lower().startswith("$includemodel"):
            continue        # re-stated below, so running this twice does not stack them up
        if line.lstrip().lower().startswith("$sequence"):
            if not seen_seq:
                seen_seq = True
                for include in includes:
                    out.append(f'$includemodel "{include}"\n')
                out.append('$sequence "bindpose" "bindpose.smd" fps 30\n')
            if args.keep_anims and '"bindpose"' not in line:
                out.append(line)
            continue
        out.append(line)

    with open(qc_path, "w", encoding="ascii") as f:
        f.writelines(out)
    print(f"  qc: {qc_path} ({total} bone names -> ValveBiped)")

    # ---------------------------------------------------------------------------------------- compile
    r = subprocess.run([studiomdl, "-game", game, "-nop4", "-quiet", qc_path],
                       capture_output=True, text=True, cwd=work)
    for line in (r.stdout + r.stderr).strip().splitlines()[-12:]:
        print("   " + line)

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
