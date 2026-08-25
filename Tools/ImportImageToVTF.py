"""
Turn an ordinary image into a Source .vtf, for the odd piece of UI art that has no Source original to import.

Writes a version 7.2 file - header, 16x16 thumbnail, image data - which is the layout every VTF had before the
resource dictionary arrived in 7.3, and the one FSourceVTFFile reads when a file has no dictionary. The offsets
below are that header exactly, packed, and match what SourceVTFFile.cpp reads field for field.

    python Tools/ImportImageToVTF.py <image> <out.vtf> [--format dxt1|dxt5|bgr|bgra] [--mips]

DXT1 is the default because it is what Source uses for most things and it costs a quarter of the bytes. It needs
both sides to be multiples of four; anything else falls back to BGRA8888 with a warning. DXT1 is a poor choice for
a smooth gradient, though - it keeps four colours per 4x4 block, which a dark fade turns into visible rings - so
use bgr for those, which is uncompressed and has no alpha channel to pay for.
"""

import argparse
import io
import os
import struct
import sys

from PIL import Image

# imageformat_declarations.h, which is the same order ESourceImageFormat is in.
FORMAT_RGBA8888 = 0
FORMAT_BGR888 = 3
FORMAT_BGRA8888 = 12
FORMAT_DXT1 = 13
FORMAT_DXT5 = 15

# vtf.h
FLAG_NOMIP = 0x0100
FLAG_NOLOD = 0x0200
FLAG_EIGHTBITALPHA = 0x2000

HEADER_SIZE = 80
DDS_HEADER_SIZE = 128


def encode_dxt(image, pixel_format):
    """One image's worth of DXT blocks, taken out of a DDS Pillow writes for us."""
    buffer = io.BytesIO()
    image.convert("RGBA").save(buffer, format="DDS", pixel_format=pixel_format)
    return buffer.getvalue()[DDS_HEADER_SIZE:]


def encode_bgr(image):
    """Three bytes a pixel, no alpha. Half again smaller than BGRA and, for an opaque image, identical."""
    rgb = image.convert("RGB").tobytes()
    out = bytearray(len(rgb))
    out[0::3] = rgb[2::3]
    out[1::3] = rgb[1::3]
    out[2::3] = rgb[0::3]
    return bytes(out)


def encode_bgra(image):
    rgba = image.convert("RGBA").tobytes()
    out = bytearray(len(rgba))
    out[0::4] = rgba[2::4]
    out[1::4] = rgba[1::4]
    out[2::4] = rgba[0::4]
    out[3::4] = rgba[3::4]
    return bytes(out)


def encode(image, fmt):
    if fmt == FORMAT_DXT1:
        return encode_dxt(image, "DXT1")
    if fmt == FORMAT_DXT5:
        return encode_dxt(image, "DXT5")
    if fmt == FORMAT_BGR888:
        return encode_bgr(image)
    return encode_bgra(image)


def key_black_to_alpha(image):
    """
    Turn a picture drawn on black into one drawn on nothing.

    Artwork that arrives as a logo on a black plate composites correctly only over black. Reading how bright a
    pixel is as how opaque it is, and dividing the colour back out by that, recovers the shape with its
    antialiased edges intact - which is the same thing as undoing a premultiply against black.
    """
    image = image.convert("RGBA")
    data = bytearray(image.tobytes())
    brightest = max((max(data[i], data[i + 1], data[i + 2]) for i in range(0, len(data), 4)), default=0)
    if brightest == 0:
        return image
    for i in range(0, len(data), 4):
        r, g, b = data[i], data[i + 1], data[i + 2]
        v = max(r, g, b)
        if v == 0:
            data[i:i + 4] = b"\0\0\0\0"
            continue
        scale = brightest / v
        data[i] = min(255, int(r * scale + 0.5))
        data[i + 1] = min(255, int(g * scale + 0.5))
        data[i + 2] = min(255, int(b * scale + 0.5))
        data[i + 3] = min(255, int(v * 255 / brightest + 0.5))
    return Image.frombytes("RGBA", image.size, bytes(data))


def mip_sizes(width, height):
    """VTF's own mip dimensions: each level is the full size shifted down, floored at one."""
    levels = []
    level = 0
    while True:
        w = max(1, width >> level)
        h = max(1, height >> level)
        levels.append((w, h))
        if w == 1 and h == 1:
            break
        level += 1
    return levels


def build_header(width, height, fmt, num_mips, flags):
    header = bytearray(HEADER_SIZE)
    struct.pack_into("<4s", header, 0, b"VTF\0")
    struct.pack_into("<II", header, 4, 7, 2)        # version 7.2
    struct.pack_into("<I", header, 12, HEADER_SIZE)
    struct.pack_into("<HH", header, 16, width, height)
    struct.pack_into("<I", header, 20, flags)
    struct.pack_into("<HH", header, 24, 1, 0)       # one frame, starting at zero
    struct.pack_into("<fff", header, 32, 0.5, 0.5, 0.5)   # reflectivity: only radiosity cares
    struct.pack_into("<f", header, 48, 1.0)         # bumpScale
    struct.pack_into("<i", header, 52, fmt)
    struct.pack_into("<B", header, 56, num_mips)
    struct.pack_into("<i", header, 57, FORMAT_DXT1)  # the thumbnail is always DXT1
    struct.pack_into("<BB", header, 61, 16, 16)
    struct.pack_into("<H", header, 63, 1)           # depth, 7.2 and up
    return bytes(header)


def convert(source_path, out_path, fmt, want_mips, key_black=False):
    image = Image.open(source_path).convert("RGBA")
    if key_black:
        image = key_black_to_alpha(image)
    width, height = image.size

    if fmt in (FORMAT_DXT1, FORMAT_DXT5) and (width % 4 or height % 4):
        print(f"  {width}x{height} is not a multiple of 4, so DXT is out; writing BGRA8888 instead")
        fmt = FORMAT_BGRA8888

    levels = mip_sizes(width, height) if want_mips else [(width, height)]
    # DXT cannot encode a level narrower than a block. The chain is cut at the first level that cannot be
    # encoded rather than having that level filtered out of it: the reader works out where each mip lives by
    # halving from the top, so what is kept has to stay a run from level zero down.
    if fmt in (FORMAT_DXT1, FORMAT_DXT5):
        kept = []
        for (w, h) in levels:
            if w % 4 or h % 4:
                break
            kept.append((w, h))
        levels = kept or [(width, height)]

    flags = 0
    if fmt in (FORMAT_BGRA8888, FORMAT_DXT5):
        flags |= FLAG_EIGHTBITALPHA
    if len(levels) == 1:
        flags |= FLAG_NOMIP | FLAG_NOLOD

    # The thumbnail Source keeps for the material system's average colour.
    thumbnail = encode_dxt(image.resize((16, 16), Image.LANCZOS), "DXT1")

    # Mips are stored smallest first (see FSourceVTFFile::ComputeMipOffset).
    body = bytearray()
    for (w, h) in reversed(levels):
        level_image = image if (w, h) == (width, height) else image.resize((w, h), Image.LANCZOS)
        body += encode(level_image, fmt)

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(build_header(width, height, fmt, len(levels), flags))
        f.write(thumbnail)
        f.write(body)

    name = {FORMAT_DXT1: "DXT1", FORMAT_DXT5: "DXT5", FORMAT_BGR888: "BGR888",
            FORMAT_BGRA8888: "BGRA8888"}[fmt]
    print(f"  {out_path}: {width}x{height} {name}, {len(levels)} mip(s), "
          f"{HEADER_SIZE + len(thumbnail) + len(body):,} bytes")


def main():
    parser = argparse.ArgumentParser(description="Convert an image to a Source VTF")
    parser.add_argument("source")
    parser.add_argument("out")
    parser.add_argument("--format", choices=["dxt1", "dxt5", "bgr", "bgra"], default="dxt1")
    parser.add_argument("--mips", action="store_true", help="write the whole mip chain, not just the top level")
    parser.add_argument("--key-black", action="store_true",
                        help="read brightness as opacity, for artwork that came on a black plate")
    args = parser.parse_args()

    fmt = {"dxt1": FORMAT_DXT1, "dxt5": FORMAT_DXT5, "bgr": FORMAT_BGR888,
           "bgra": FORMAT_BGRA8888}[args.format]
    convert(args.source, args.out, fmt, args.mips, args.key_black)
    return 0


if __name__ == "__main__":
    sys.exit(main())
