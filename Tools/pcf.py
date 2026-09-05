"""
Reads a Source 1 particle collection (.pcf) - a binary DMX file - without the engine.

The engine has its own reader in C++ (Formats/SourcePCFFile.cpp); this one exists so the import tools - and anyone
poking around from a shell - can see what a PCF holds, which operators a set of files uses, and which materials they
need, without launching the game.

    python Tools/pcf.py list <file.pcf | dir.vpk> ...        systems in each file, with their operators
    python Tools/pcf.py ops <file.pcf | dir.vpk> ...         every functionName used, with a count
    python Tools/pcf.py materials <file.pcf | dir.vpk> ...   every material referenced
    python Tools/pcf.py dump <file.pcf> <system>             every attribute of one system and its operators

A VPK argument reads every particles/*.pcf inside it. The DMX container is: a header line, a NUL, a string table,
an element dictionary (type, name, GUID) and then one attribute list per element, in dictionary order. Binary
encodings 1-5 differ only in whether names come from the table and how wide the table indices are.
"""

import os
import struct
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vpk import VPK

# DmAttributeType_t
AT_ELEMENT, AT_INT, AT_FLOAT, AT_BOOL, AT_STRING, AT_VOID, AT_TIME, AT_COLOR = range(1, 9)
AT_VECTOR2, AT_VECTOR3, AT_VECTOR4, AT_QANGLE, AT_QUATERNION, AT_VMATRIX = range(9, 15)
AT_FIRST_ARRAY = 15
AT_ARRAY_OFFSET = 14


class Element:
    def __init__(self, type_name, name, guid):
        self.type = type_name
        self.name = name
        self.guid = guid
        self.attributes = {}   # name -> value (Element refs are Element objects, arrays are lists)

    def get(self, name, default=None):
        return self.attributes.get(name, default)


class PCF:
    def __init__(self, data):
        self.elements = []
        self._parse(data)

    # -- container --------------------------------------------------------------------------------------------

    def _parse(self, data):
        end = data.index(b"\x00")
        header = data[:end].decode("ascii", "replace")
        if "<!-- dmx encoding binary" not in header:
            raise ValueError("not a binary DMX file: %r" % header)
        self.version = int(header.split("binary")[1].split()[0])
        pos = end + 1
        v = self.version

        strings = []
        if v >= 2:
            if v >= 4:
                (count,) = struct.unpack_from("<i", data, pos)
                pos += 4
            else:
                (count,) = struct.unpack_from("<h", data, pos)
                pos += 2
            for _ in range(count):
                z = data.index(b"\x00", pos)
                strings.append(data[pos:z].decode("utf-8", "replace"))
                pos = z + 1
        self.strings = strings

        def index():
            nonlocal pos
            if v >= 5:
                (i,) = struct.unpack_from("<i", data, pos)
                pos += 4
            else:
                (i,) = struct.unpack_from("<H", data, pos)
                pos += 2
            return i

        def cstr():
            nonlocal pos
            z = data.index(b"\x00", pos)
            s = data[pos:z].decode("utf-8", "replace")
            pos = z + 1
            return s

        (n_elements,) = struct.unpack_from("<i", data, pos)
        pos += 4
        for _ in range(n_elements):
            type_name = strings[index()] if v >= 2 else cstr()
            name = strings[index()] if v >= 4 else cstr()
            guid = data[pos:pos + 16]
            pos += 16
            self.elements.append(Element(type_name, name, guid))

        for element in self.elements:
            (n_attributes,) = struct.unpack_from("<i", data, pos)
            pos += 4
            for _ in range(n_attributes):
                attr_name = strings[index()] if v >= 2 else cstr()
                attr_type = data[pos]
                pos += 1
                value, pos = self._read_value(data, pos, attr_type, strings, v)
                element.attributes[attr_name] = value

    def _read_value(self, data, pos, attr_type, strings, v):
        if attr_type >= AT_FIRST_ARRAY:
            (count,) = struct.unpack_from("<i", data, pos)
            pos += 4
            items = []
            inner = attr_type - AT_ARRAY_OFFSET
            for _ in range(count):
                if inner == AT_STRING:
                    # Array strings are always inline, whatever the version.
                    z = data.index(b"\x00", pos)
                    items.append(data[pos:z].decode("utf-8", "replace"))
                    pos = z + 1
                else:
                    item, pos = self._read_value(data, pos, inner, strings, v)
                    items.append(item)
            return items, pos

        if attr_type == AT_ELEMENT:
            (i,) = struct.unpack_from("<i", data, pos)
            pos += 4
            if i == -2:
                z = data.index(b"\x00", pos)
                pos = z + 1
                return None, pos
            return (self.elements[i] if i >= 0 else None), pos
        if attr_type == AT_INT:
            return struct.unpack_from("<i", data, pos)[0], pos + 4
        if attr_type == AT_FLOAT:
            return struct.unpack_from("<f", data, pos)[0], pos + 4
        if attr_type == AT_BOOL:
            return data[pos] != 0, pos + 1
        if attr_type == AT_STRING:
            if v >= 4:
                if v >= 5:
                    (i,) = struct.unpack_from("<i", data, pos)
                    pos += 4
                else:
                    (i,) = struct.unpack_from("<H", data, pos)
                    pos += 2
                return strings[i], pos
            z = data.index(b"\x00", pos)
            return data[pos:z].decode("utf-8", "replace"), z + 1
        if attr_type == AT_VOID:
            (n,) = struct.unpack_from("<i", data, pos)
            return data[pos + 4:pos + 4 + n], pos + 4 + n
        if attr_type == AT_TIME:
            return struct.unpack_from("<i", data, pos)[0] / 10000.0, pos + 4
        if attr_type == AT_COLOR:
            return tuple(data[pos:pos + 4]), pos + 4
        if attr_type == AT_VECTOR2:
            return struct.unpack_from("<2f", data, pos), pos + 8
        if attr_type in (AT_VECTOR3, AT_QANGLE):
            return struct.unpack_from("<3f", data, pos), pos + 12
        if attr_type in (AT_VECTOR4, AT_QUATERNION):
            return struct.unpack_from("<4f", data, pos), pos + 16
        if attr_type == AT_VMATRIX:
            return struct.unpack_from("<16f", data, pos), pos + 64
        raise ValueError("unknown attribute type %d at %d" % (attr_type, pos))

    # -- particle view -----------------------------------------------------------------------------------------

    def definitions(self):
        """Every DmeParticleSystemDefinition in the file, root ones first, then those only reachable as children."""
        root = self.elements[0]
        found = []
        if root.type.lower() == "dmeparticlesystemdefinition":
            found.append(root)
        else:
            for value in root.attributes.values():
                if isinstance(value, list):
                    found.extend(e for e in value if isinstance(e, Element) and e.type.lower() == "dmeparticlesystemdefinition")
        seen = set(id(e) for e in found)
        for element in self.elements:
            if element.type.lower() == "dmeparticlesystemdefinition" and id(element) not in seen:
                found.append(element)
                seen.add(id(element))
        return found


CATEGORIES = ("renderers", "operators", "initializers", "emitters", "forces", "constraints")


def operators_of(definition):
    for category in CATEGORIES:
        for op in definition.get(category, []) or []:
            if isinstance(op, Element):
                yield category, op.get("functionName", op.name)


def load_all(paths):
    """Yields (label, PCF) for each .pcf file, or each particles/*.pcf inside a VPK."""
    for path in paths:
        if path.lower().endswith(".vpk"):
            vpk = VPK(path)
            for name in sorted(vpk.entries):
                if name.lower().startswith("particles/") and name.lower().endswith(".pcf"):
                    yield "%s:%s" % (os.path.basename(path), name), PCF(vpk.read(name))
        else:
            with open(path, "rb") as f:
                yield path, PCF(f.read())


def fmt(value):
    if isinstance(value, Element):
        return "<%s %s>" % (value.type, value.name)
    if isinstance(value, list):
        return "[%s]" % ", ".join(fmt(v) for v in value)
    if isinstance(value, float):
        return "%g" % value
    if isinstance(value, tuple):
        return "(%s)" % " ".join("%g" % v for v in value)
    return repr(value)


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    command, args = argv[1], argv[2:]

    if command == "list":
        for label, pcf in load_all(args):
            print("== %s (binary %d)" % (label, pcf.version))
            for d in pcf.definitions():
                children = [c.get("child").name for c in d.get("children", []) or [] if isinstance(c, Element) and c.get("child")]
                print("  %-40s max %-5s %s%s" % (d.name, d.get("max_particles", 1000), d.get("material", ""),
                                                  ("  children: " + ", ".join(children)) if children else ""))
                for category, name in operators_of(d):
                    print("      %-13s %s" % (category, name))
    elif command == "ops":
        counts = Counter()
        for _, pcf in load_all(args):
            for d in pcf.definitions():
                for category, name in operators_of(d):
                    counts[(category, name)] += 1
        for (category, name), n in sorted(counts.items(), key=lambda kv: (kv[0][0], -kv[1])):
            print("%5d  %-13s %s" % (n, category, name))
    elif command == "materials":
        counts = Counter()
        for _, pcf in load_all(args):
            for d in pcf.definitions():
                counts[d.get("material", "")] += 1
        for name, n in sorted(counts.items()):
            print("%5d  %s" % (n, name))
    elif command == "dump":
        path, wanted = args[0], args[1]
        for _, pcf in load_all([path]):
            for d in pcf.definitions():
                if d.name.lower() != wanted.lower():
                    continue
                print("== %s" % d.name)
                for key, value in d.attributes.items():
                    if key in CATEGORIES or key == "children":
                        continue
                    print("  %-50s %s" % (key, fmt(value)))
                for category in CATEGORIES:
                    for op in d.get(category, []) or []:
                        print("  -- %s: %s" % (category, op.get("functionName", op.name)))
                        for key, value in op.attributes.items():
                            if key == "functionName":
                                continue
                            print("      %-46s %s" % (key, fmt(value)))
                for child in d.get("children", []) or []:
                    print("  -- child: %s delay %g" % (child.get("child").name if child.get("child") else "?", child.get("delay", 0.0)))
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
