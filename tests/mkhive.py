#!/usr/bin/env python3
"""mkhive.py - write a small, valid Windows registry hive (regf).

Test fixture generator: builds SYSTEM-style hives with exactly the keys and
values a test wants, without needing a Windows machine.

    from mkhive import Hive
    h = Hive()
    h.set(r"Select", {"Current": ("dword", 1), "Default": ("dword", 1)})
    h.set(r"ControlSet001\\Services\\Tcpip\\Parameters\\Interfaces\\{GUID}",
          {"EnableDHCP": ("dword", 0),
           "IPAddress": ("multi", ["192.168.77.10"])})
    open("SYSTEM", "wb").write(h.build())

Supported value kinds: "sz", "expand", "multi", "dword", "qword", "binary".
"""

import struct

HBIN_OFFSET = 0x1000
HBIN_HEADER = 0x20
ALIGN = 8
FILE_TIME = 0x01CA000000000000      # arbitrary but plausible


def _align(n):
    return (n + ALIGN - 1) // ALIGN * ALIGN


class Key:
    def __init__(self, name, parent=None):
        self.name = name
        self.parent = parent
        self.subkeys = []
        self.values = []            # list of (name, kind, payload)
        self.nk = 0
        self.name_raw = b""
        self.comp = True
        self.subkey_list = 0
        self.value_list = 0
        self.subkey_count = 0
        self.value_count = 0


class Hive:
    def __init__(self, root_name="ROOT"):
        self.root = Key(root_name)
        self._index = {(): self.root}

    # ---------------------------------------------------------------- editing

    def key(self, path):
        """Create (or return) a key by backslash path."""
        parts = [p for p in path.split("\\") if p]
        node = self.root
        for i, part in enumerate(parts):
            trail = tuple(parts[:i + 1])
            if trail in self._index:
                node = self._index[trail]
                continue
            child = Key(part, parent=node)
            node.subkeys.append(child)
            self._index[trail] = child
            node = child
        return node

    def set(self, path, values):
        node = self.key(path)
        for name, (kind, value) in values.items():
            node.values.append((name, kind, self._encode(kind, value)))
        return node

    @staticmethod
    def _encode(kind, value):
        if kind in ("sz", "expand"):
            return value.encode("utf-16-le") + b"\x00\x00"
        if kind == "multi":
            out = b""
            for part in value:
                out += part.encode("utf-16-le") + b"\x00\x00"
            return out + b"\x00\x00"
        if kind == "dword":
            return struct.pack("<I", value)
        if kind == "qword":
            return struct.pack("<Q", value)
        if kind == "binary":
            return bytes(value)
        raise ValueError("unknown value kind: %r" % kind)

    # ----------------------------------------------------------------- build

    def build(self):
        self._prepare(self.root)
        self._allocated = 0
        self._assign(self.root)
        size = HBIN_HEADER + self._allocated
        size = max(size, 4096)
        size = (size + 4095) // 4096 * 4096

        img = bytearray(HBIN_OFFSET + size)
        # base block
        img[0:4] = b"regf"
        struct.pack_into("<I", img, 0x04, 1)
        struct.pack_into("<I", img, 0x08, 1)
        struct.pack_into("<Q", img, 0x0C, FILE_TIME)
        struct.pack_into("<I", img, 0x14, 1)       # major
        struct.pack_into("<I", img, 0x18, 5)       # minor
        struct.pack_into("<I", img, 0x1C, 0)       # primary
        struct.pack_into("<I", img, 0x20, 1)       # direct memory load
        struct.pack_into("<I", img, 0x24, self.root.nk)
        struct.pack_into("<I", img, 0x28, size)
        struct.pack_into("<I", img, 0x2C, 1)       # clustering factor
        img[0x30:0x30 + 64] = "WRECOVERY-TEST".encode("utf-16-le")[:64]

        # hbin header
        img[HBIN_OFFSET:HBIN_OFFSET + 4] = b"hbin"
        struct.pack_into("<I", img, HBIN_OFFSET + 0x04, 0)
        struct.pack_into("<I", img, HBIN_OFFSET + 0x08, size)

        self._emit(self.root, img, HBIN_OFFSET)

        # trailing free cell
        used = HBIN_HEADER + self._allocated
        free = size - used
        if free >= 8:
            struct.pack_into("<i", img, HBIN_OFFSET + used, -free)
        return bytes(img)

    # -------------------------------------------------------------- internals

    def _prepare(self, key):
        key.comp = all(ord(c) < 128 for c in key.name)
        key.name_raw = (key.name.encode("latin-1") if key.comp
                        else key.name.encode("utf-16-le"))
        key.subkey_count = len(key.subkeys)
        key.value_count = len(key.values)
        for child in key.subkeys:
            self._prepare(child)

    @staticmethod
    def _size(key):
        total = 0
        total += _align(4 + 0x4C + len(key.name_raw))              # nk
        if key.subkeys:
            total += _align(4 + 4 + 8 * len(key.subkeys))          # lf
        if key.values:
            total += _align(4 + 4 * len(key.values))               # value list
        for name, kind, data in key.values:
            nlen = len(name.encode("latin-1"))
            total += _align(4 + 0x14 + nlen)                       # vk
            if len(data) > 4:
                total += _align(4 + len(data))                     # data cell
        for child in key.subkeys:
            total += Hive._size(child)
        return total

    def _alloc(self, size):
        off = HBIN_HEADER + self._allocated
        self._allocated += _align(size)
        return off

    def _emit(self, key, img, base):
        self._write(key, img, base)

    def _assign(self, key):
        key.nk = self._alloc(4 + 0x4C + len(key.name_raw))
        if key.subkeys:
            key.subkey_list = self._alloc(4 + 4 + 8 * len(key.subkeys))
        if key.values:
            key.value_list = self._alloc(4 + 4 * len(key.values))
        key._vk_cells = []
        for name, kind, data in key.values:
            nlen = len(name.encode("latin-1"))
            vk = self._alloc(4 + 0x14 + nlen)
            data_cell = 0
            if len(data) > 4:
                data_cell = self._alloc(4 + len(data))
            key._vk_cells.append((vk, data_cell))
        for child in key.subkeys:
            self._assign(child)

    def _write(self, key, img, base):
        def cell_ptr(idx):
            return base + idx

        # ---- nk
        nk = cell_ptr(key.nk)
        struct.pack_into("<i", img, nk, -_align(4 + 0x4C + len(key.name_raw)))
        nk += 4
        img[nk:nk + 2] = b"nk"
        struct.pack_into("<H", img, nk + 0x02, 0x2C if key.comp else 0x2C)
        struct.pack_into("<Q", img, nk + 0x04, FILE_TIME)
        struct.pack_into("<I", img, nk + 0x10,
                         0xFFFFFFFF if key.parent is None else key.parent.nk)
        struct.pack_into("<I", img, nk + 0x14, len(key.subkeys))
        struct.pack_into("<I", img, nk + 0x18, 0)
        struct.pack_into("<I", img, nk + 0x1C, key.subkey_list or 0xFFFFFFFF)
        struct.pack_into("<I", img, nk + 0x20, 0xFFFFFFFF)
        struct.pack_into("<I", img, nk + 0x24, len(key.values))
        struct.pack_into("<I", img, nk + 0x28, key.value_list or 0xFFFFFFFF)
        struct.pack_into("<I", img, nk + 0x2C, 0xFFFFFFFF)
        struct.pack_into("<I", img, nk + 0x30, 0xFFFFFFFF)
        struct.pack_into("<H", img, nk + 0x48, len(key.name_raw))
        struct.pack_into("<H", img, nk + 0x4A, 0)
        img[nk + 0x4C:nk + 0x4C + len(key.name_raw)] = key.name_raw

        # ---- subkey list (lf)
        if key.subkeys:
            lst = cell_ptr(key.subkey_list)
            struct.pack_into("<i", img, lst, -_align(4 + 4 + 8 * len(key.subkeys)))
            lst += 4
            img[lst:lst + 2] = b"lf"
            struct.pack_into("<H", img, lst + 2, len(key.subkeys))
            for i, child in enumerate(key.subkeys):
                entry = lst + 4 + i * 8
                struct.pack_into("<I", img, entry, child.nk)
                struct.pack_into("<I", img, entry + 4, self._lf_hash(child.name))

        # ---- value list + vk cells
        if key.values:
            vl = cell_ptr(key.value_list)
            struct.pack_into("<i", img, vl, -_align(4 + 4 * len(key.values)))
            vl += 4
            for i, (vk_idx, _) in enumerate(key._vk_cells):
                struct.pack_into("<I", img, vl + i * 4, vk_idx)

        for (vk_idx, data_idx), (name, kind, data) in zip(key._vk_cells,
                                                          key.values):
            vk = cell_ptr(vk_idx)
            name_raw = name.encode("latin-1")
            struct.pack_into("<i", img, vk,
                             -_align(4 + 0x14 + len(name_raw)))
            vk += 4
            img[vk:vk + 2] = b"vk"
            struct.pack_into("<H", img, vk + 0x02, len(name_raw))
            if len(data) <= 4:
                struct.pack_into("<I", img, vk + 0x04, 0x80000000 | len(data))
                struct.pack_into("<I", img, vk + 0x08, 0)
                img[vk + 0x08:vk + 0x08 + len(data)] = data
            else:
                struct.pack_into("<I", img, vk + 0x04, len(data))
                struct.pack_into("<I", img, vk + 0x08, data_idx)
            struct.pack_into("<I", img, vk + 0x0C, self._type(kind))
            struct.pack_into("<H", img, vk + 0x10, 0x0001)   # name is ascii
            struct.pack_into("<H", img, vk + 0x12, 0)
            img[vk + 0x14:vk + 0x14 + len(name_raw)] = name_raw

            if data_idx:
                dc = cell_ptr(data_idx)
                struct.pack_into("<i", img, dc, -_align(4 + len(data)))
                img[dc + 4:dc + 4 + len(data)] = data

        for child in key.subkeys:
            self._write(child, img, base)

    @staticmethod
    def _type(kind):
        return {"sz": 1, "expand": 2, "binary": 3, "dword": 4,
                "multi": 7, "qword": 11}[kind]

    @staticmethod
    def _lf_hash(name):
        h = 0
        for ch in name[:4]:
            h = (h * 37 + ord(ch.upper())) & 0xFFFFFFFF
        return h


if __name__ == "__main__":
    import sys

    if len(sys.argv) < 2:
        raise SystemExit("usage: mkhive.py OUT.bin")
    h = Hive()
    h.set(r"Select", {"Current": ("dword", 1), "Default": ("dword", 1)})
    open(sys.argv[1], "wb").write(h.build())
    print("mkhive: %s" % sys.argv[1])