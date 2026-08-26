"""
Reads a Source 1 VPK directory (the `_dir.vpk` of a multi-chunk set, or a single-file one).

The engine has its own reader in C++; this one exists so the import tools - and anyone poking around from a
shell - can list and extract without launching the game.

    python Tools/vpk.py list <dir.vpk> [substring]
    python Tools/vpk.py extract <dir.vpk> <path-in-vpk> <out-file>

The directory is three nested trees - extension, then folder, then filename - each a run of NUL-terminated
strings ended by an empty one, with a fixed entry record after every filename. A file's bytes live either
inline in the directory (the preload block) or in the numbered chunk its entry names, `_NNN.vpk` beside the
directory; archive index 0x7fff means the data sits in this file after the tree.
"""

import os
import struct
import sys

SIGNATURE = 0x55AA1234


class VPKEntry:
    def __init__(self, path, crc, preload, archive_index, offset, length):
        self.path = path
        self.crc = crc
        self.preload = preload
        self.archive_index = archive_index
        self.offset = offset
        self.length = length

    @property
    def size(self):
        return self.length + len(self.preload)


class VPK:
    def __init__(self, dir_path):
        self.dir_path = dir_path
        self.entries = {}
        self._read_directory()

    def _read_directory(self):
        with open(self.dir_path, "rb") as f:
            data = f.read()

        sig, version, tree_size = struct.unpack_from("<III", data, 0)
        if sig != SIGNATURE:
            raise ValueError(f"{self.dir_path}: not a VPK (signature {sig:#x})")
        # v1 is 12 bytes of header; v2 adds four more lengths after the tree size.
        header = 12 if version == 1 else 28
        self.data_offset = header + tree_size

        pos = header
        end = header + tree_size

        def cstr():
            nonlocal pos
            z = data.index(b"\x00", pos)
            s = data[pos:z].decode("ascii", "replace")
            pos = z + 1
            return s

        while pos < end:
            ext = cstr()
            if not ext:
                break
            while True:
                folder = cstr()
                if not folder:
                    break
                while True:
                    name = cstr()
                    if not name:
                        break
                    crc, preload_len, archive_index, offset, length, term = \
                        struct.unpack_from("<IHHIIH", data, pos)
                    pos += 18
                    preload = data[pos:pos + preload_len]
                    pos += preload_len

                    path = f"{name}.{ext}" if ext != " " else name
                    if folder not in ("", " "):
                        path = f"{folder}/{path}"
                    self.entries[path.lower()] = VPKEntry(
                        path, crc, preload, archive_index, offset, length)

    def read(self, path):
        entry = self.entries[path.lower().replace("\\", "/")]
        if entry.length == 0:
            return entry.preload
        if entry.archive_index == 0x7FFF:
            source, offset = self.dir_path, self.data_offset + entry.offset
        else:
            base = self.dir_path[:-len("_dir.vpk")]
            source, offset = f"{base}_{entry.archive_index:03d}.vpk", entry.offset
        with open(source, "rb") as f:
            f.seek(offset)
            return entry.preload + f.read(entry.length)


def main():
    if len(sys.argv) < 3:
        print(__doc__.strip())
        return 1
    mode, vpk_path = sys.argv[1], sys.argv[2]
    vpk = VPK(vpk_path)

    if mode == "list":
        needle = sys.argv[3].lower() if len(sys.argv) > 3 else ""
        n = 0
        for key in sorted(vpk.entries):
            if needle in key:
                e = vpk.entries[key]
                print(f"{e.size:>10}  {e.path}")
                n += 1
        print(f"-- {n} of {len(vpk.entries)} entries", file=sys.stderr)
    elif mode == "extract":
        out = sys.argv[4]
        blob = vpk.read(sys.argv[3])
        os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
        with open(out, "wb") as f:
            f.write(blob)
        print(f"wrote {out} ({len(blob)} bytes)")
    else:
        print(f"unknown mode '{mode}'", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
