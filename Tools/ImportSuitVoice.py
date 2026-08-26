#!/usr/bin/env python3
"""
Imports the HEV suit voice - Black Mesa's recording of it - out of an owned install.

The suit voice is not one sound per line. It is Half-Life's sentence system: sentences.txt names a sentence
("HEV_DMG4") and gives the words it is built from ("boop, boop, boop, minor_fracture"), each word its own wav in
sound/hev_vox, with a parameter block that shifts pitch or volume. The engine plays the words back to back. So
this tool takes both halves - every hev_vox word, and the sentence file that spells out how they go together -
and drops them into the half-life content pack.

  python Tools/ImportSuitVoice.py                          # finds Black Mesa in any Steam library
  python Tools/ImportSuitVoice.py --bms "D:/.../bms" --out "D:/.../Mods/lambda/plugins/half-life"

Both halves are Crowbar Collective's, so both are gitignored where they land, the way the footstep recordings
are; a fresh clone runs without them and the suit simply says nothing. Nothing here is redistributed - it is
read out of the install on the machine that owns it.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vpk import VPK


def find_black_mesa():
    """Black Mesa's bms/ folder, in whichever Steam library holds it."""
    roots = []
    for drive in "CDEFGH":
        roots.append(f"{drive}:/Program Files (x86)/Steam/steamapps/common")
        roots.append(f"{drive}:/SteamLibrary/steamapps/common")
        roots.append(f"{drive}:/Steam/steamapps/common")
    for root in roots:
        candidate = os.path.join(root, "Black Mesa", "bms")
        if os.path.isdir(candidate):
            return candidate
    return None


def default_out():
    """The half-life content pack, via GameDir.txt beside the .uproject."""
    here = os.path.dirname(os.path.abspath(__file__))
    game_dir_file = os.path.join(os.path.dirname(here), "GameDir.txt")
    if os.path.isfile(game_dir_file):
        # One path, with or without quotes; # comments and blank lines are ignored, as the .bat tools do.
        with open(game_dir_file, encoding="utf-8") as f:
            for line in f:
                line = line.strip().strip('"')
                if line and not line.startswith("#"):
                    return os.path.join(line, "Mods", "lambda", "plugins", "half-life")
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bms", help="Black Mesa's bms folder (default: found in any Steam library)")
    ap.add_argument("--out", help="content pack to write into (default: from GameDir.txt)")
    args = ap.parse_args()

    bms = args.bms or find_black_mesa()
    if not bms or not os.path.isdir(bms):
        sys.exit("Black Mesa not found - pass --bms <path to bms folder>")
    out = args.out or default_out()
    if not out:
        sys.exit("no output folder - pass --out, or put the game path in GameDir.txt")

    vo = VPK(os.path.join(bms, "bms_sound_vo_english_dir.vpk"))
    misc = VPK(os.path.join(bms, "bms_misc_dir.vpk"))

    # Every word of the suit's vocabulary.
    sound_dir = os.path.join(out, "sound", "hev_vox")
    os.makedirs(sound_dir, exist_ok=True)
    words = [k for k in vo.entries if k.startswith("sound/hev_vox/") and k.endswith(".wav")]
    for key in sorted(words):
        name = os.path.basename(key)
        with open(os.path.join(sound_dir, name), "wb") as f:
            f.write(vo.read(key))
    print(f"{len(words)} words -> {sound_dir}")

    # And how they go together.
    scripts_dir = os.path.join(out, "scripts")
    os.makedirs(scripts_dir, exist_ok=True)
    sentences = misc.read("scripts/sentences.txt")
    with open(os.path.join(scripts_dir, "sentences.txt"), "wb") as f:
        f.write(sentences)
    n = sum(1 for line in sentences.decode("ascii", "replace").splitlines()
            if line.strip() and not line.strip().startswith("//"))
    print(f"{n} sentences -> {os.path.join(scripts_dir, 'sentences.txt')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
