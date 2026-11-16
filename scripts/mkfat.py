#!/usr/bin/env python3
"""mkfat.py - build a FAT16 filesystem image from a directory tree.

Used for two things:
  * the El Torito UEFI boot image that carries EFI/BOOT/BOOTX64.EFI
  * throw-away test volumes for the QEMU boot tests

No mtools, no root, no loop devices: pure python.

    mkfat.py OUT.img SRCDIR [--label NAME] [--size BYTES]

The image is grown to fit the content unless --size is given.
Long file names are supported (needed for test volumes with ProgramData etc.).
"""

import argparse
import os
import struct
import sys

SECTOR = 512
DIRENT = 32
ATTR_READONLY = 0x01
ATTR_HIDDEN = 0x02
ATTR_SYSTEM = 0x04
ATTR_VOLUME_ID = 0x08
ATTR_DIRECTORY = 0x10
ATTR_ARCHIVE = 0x20
ATTR_LFN = 0x0F

INVALID_SHORT = set(' "*/:<>?\\|+,;=[]')


class Node:
    def __init__(self, name, is_dir, data=None, children=None):
        self.name = name
        self.is_dir = is_dir
        self.data = data or b""
        self.children = children or []
        self.first_cluster = 0
        self.size = 0


def scan(path):
    name = os.path.basename(path.rstrip("/")) or "ROOT"
    if os.path.isdir(path):
        kids = []
        for entry in sorted(os.listdir(path)):
            if entry.startswith("."):
                continue
            kids.append(scan(os.path.join(path, entry)))
        return Node(name, True, children=kids)
    with open(path, "rb") as f:
        return Node(name, False, data=f.read())


# --------------------------------------------------------------- FAT names --

def lfn_checksum(short_name_bytes):
    s = 0
    for b in short_name_bytes:
        s = (((s & 1) << 7) + (s >> 1) + b) & 0xFF
    return s


def make_short_name(name, taken):
    """Return (11-byte short name, needs_lfn)."""
    up = name.upper()
    base, dot, ext = up.rpartition(".")
    if not dot:
        base, ext = up, ""

    def clean(s):
        out = "".join(c for c in s if c not in INVALID_SHORT and 32 < ord(c) < 127)
        return out

    base, ext = clean(base), clean(ext)[:3]
    short = (base[:8].ljust(8) + ext.ljust(3)).encode("ascii", "replace")
    fits = (
        base == base[:8]
        and len(base) <= 8
        and len(ext) <= 3
        and "." not in base
        and base
        and name.upper() == name
        and name == base + ("." + ext if ext else "")
    )
    if fits and short not in taken:
        taken.add(short)
        return short, False

    # need a generated 8.3 name: NAME~N.EXT
    for n in range(1, 1000000):
        tail = "~%d" % n
        cand = clean(base)[: 8 - len(tail)] + tail
        short = (cand.ljust(8) + ext.ljust(3)).encode("ascii", "replace")
        if short not in taken:
            taken.add(short)
            return short, True
    raise RuntimeError("cannot generate short name for %r" % name)


def lfn_entries(name, short, checksum):
    """Build the long-file-name entries (without the short entry)."""
    chars = name.encode("utf-16-le")
    units = [chars[i : i + 2] for i in range(0, len(chars), 2)]
    # spec: terminate with 0x0000, then pad the rest of the slot with 0xFFFF
    units.append(b"\x00\x00")
    while len(units) % 13:
        units.append(b"\xff\xff")
    chunks = [units[i : i + 13] for i in range(0, len(units), 13)]
    entries = []
    for idx, chunk in enumerate(chunks):
        seq = idx + 1
        if idx == len(chunks) - 1:
            seq |= 0x40
        raw = b"".join(chunk)
        e = bytearray(DIRENT)
        e[0] = seq
        e[1:11] = raw[0:10]
        e[11] = ATTR_LFN
        e[12] = 0
        e[13] = checksum
        e[14:26] = raw[10:22]
        e[26:28] = b"\x00\x00"
        e[28:32] = raw[22:26]
        entries.append(bytes(e))
    entries.reverse()
    return entries


def dirent(short, attr, cluster, size, date=0x5A21, time=0x0000):
    e = bytearray(DIRENT)
    e[0:11] = short
    e[11] = attr
    e[12] = 0
    e[13] = 0
    e[14] = 0
    e[15] = 0
    struct.pack_into("<H", e, 16, 0)          # write time
    struct.pack_into("<H", e, 18, date)       # write date
    struct.pack_into("<H", e, 20, cluster >> 16)
    struct.pack_into("<H", e, 22, 0)          # write time
    struct.pack_into("<H", e, 24, date)
    struct.pack_into("<H", e, 26, cluster & 0xFFFF)
    struct.pack_into("<I", e, 28, size)
    return bytes(e)


def build_dir_entries(node, parent_cluster, taken_by_dir):
    """Return raw directory bytes (no padding) for a directory node."""
    out = bytearray()
    if parent_cluster is not None:
        # subdirectories start with "." and ".."; the root directory does not
        out += dirent(b"." .ljust(11, b" "), ATTR_DIRECTORY, node.first_cluster, 0)
        out += dirent(b"..".ljust(11, b" "), ATTR_DIRECTORY, parent_cluster, 0)
    for child in node.children:
        taken = taken_by_dir.setdefault(id(node), set())
        short, needs_lfn = make_short_name(child.name, taken)
        if needs_lfn:
            out += b"".join(lfn_entries(child.name, short, lfn_checksum(short)))
        if child.is_dir:
            out += dirent(short, ATTR_DIRECTORY, child.first_cluster, 0)
        else:
            out += dirent(short, ATTR_ARCHIVE, child.first_cluster, len(child.data))
    return bytes(out)


# ------------------------------------------------------------------ builder --

def choose_cluster_size(total_bytes):
    """Pick (sectors_per_cluster, fat_bits) for the given content size."""
    for sec_per_clus in (1, 2, 4, 8, 16, 32, 64):
        clus = total_bytes // (sec_per_clus * SECTOR) + 8
        if clus < 4085:
            return sec_per_clus, 12
        if clus <= 65524:
            return sec_per_clus, 16
    raise SystemExit("mkfat: content does not fit a FAT16 volume")


def layout(clusters_needed, sec_per_clus, fat_bits, size):
    """Return (fat_sectors, root_entries, root_sectors, data_start, total_sectors,
    clusters) for the given geometry, honouring a minimum image size."""
    while True:
        if fat_bits == 12:
            fat_bytes_len = (clusters_needed + 2) * 3 // 2 + 3
            root_entries = 224
        else:
            fat_bytes_len = (clusters_needed + 2) * 2
            root_entries = 512
        fat_sectors = (fat_bytes_len + SECTOR - 1) // SECTOR
        root_sectors = (root_entries * DIRENT + SECTOR - 1) // SECTOR
        data_start = 1 + 2 * fat_sectors + root_sectors
        total_sectors = data_start + (clusters_needed - 2) * sec_per_clus
        if size:
            total_sectors = max(total_sectors, (size + SECTOR - 1) // SECTOR)
        clusters = (total_sectors - data_start) // sec_per_clus + 2
        if clusters <= clusters_needed:
            return (fat_sectors, root_entries, root_sectors, data_start,
                    total_sectors, clusters)
        clusters_needed = clusters


def build(root, label="WREBOOT", size=None):
    dirs = []
    files = []

    def collect(n, parent=None):
        n.parent = parent
        if n.is_dir:
            dirs.append(n)
            for c in n.children:
                collect(c, n)
        else:
            files.append(n)

    collect(root)

    content = sum(len(f.data) for f in files) + 4096 * len(dirs)
    sec_per_clus, fat_bits = choose_cluster_size(max(content, 1))
    cluster_bytes = sec_per_clus * SECTOR

    for attempt in range(10):
        # ---- how many clusters does each directory need?
        for d in dirs:
            need = 0 if d is root else 2      # "." and ".."
            for c in d.children:
                need += 2                      # worst case: lfn + entry
            d._reserved = max(1, (need * DIRENT + cluster_bytes - 1) // cluster_bytes)

        # ---- assign clusters: directories first, then file data
        next_clus = 2
        for d in dirs:
            d.first_cluster = next_clus
            next_clus += d._reserved
        for f in files:
            n = max(1, (len(f.data) + cluster_bytes - 1) // cluster_bytes)
            f.first_cluster = next_clus
            next_clus += n
        clusters_needed = next_clus

        fat_sectors, root_entries, root_sectors, data_start, total_sectors,             clusters_needed = layout(clusters_needed, sec_per_clus, fat_bits, size)

        new_sec, new_bits = choose_cluster_size(total_sectors * SECTOR)
        if new_sec == sec_per_clus and new_bits == fat_bits:
            break
        sec_per_clus, fat_bits = new_sec, new_bits
        cluster_bytes = sec_per_clus * SECTOR
    else:
        raise SystemExit("mkfat: could not settle on a geometry")

    total_bytes = total_sectors * SECTOR
    img = bytearray(total_bytes)

    # ---- FAT
    fat = [0] * (clusters_needed + 2)
    fat[0] = 0xFF8 if fat_bits == 12 else 0xFFF8
    fat[1] = 0xFFF

    def chain(start, count):
        for i in range(count):
            fat[start + i] = 0xFFF if (i == count - 1 and fat_bits == 12) else \
                            (0xFFFF if i == count - 1 else start + i + 1)

    for d in dirs:
        chain(d.first_cluster, d._reserved)
    for f in files:
        n = max(1, (len(f.data) + cluster_bytes - 1) // cluster_bytes)
        chain(f.first_cluster, n)

    fat_bytes = bytearray(fat_sectors * SECTOR)
    if fat_bits == 12:
        for i, v in enumerate(fat):
            v &= 0xFFF
            off12 = i * 3 // 2
            if i & 1:
                fat_bytes[off12] = (fat_bytes[off12] & 0x0F) | ((v << 4) & 0xF0)
                fat_bytes[off12 + 1] = (v >> 4) & 0xFF
            else:
                fat_bytes[off12] = v & 0xFF
                fat_bytes[off12 + 1] = (fat_bytes[off12 + 1] & 0xF0) | ((v >> 8) & 0x0F)
    else:
        for i, v in enumerate(fat):
            struct.pack_into("<H", fat_bytes, i * 2, v)

    for i in range(2):
        off = (1 + i * fat_sectors) * SECTOR
        img[off : off + len(fat_bytes)] = fat_bytes

    # ---- directories
    taken_by_dir = {}
    root_off = (1 + 2 * fat_sectors) * SECTOR
    root_entries_raw = build_dir_entries(root, None, taken_by_dir)
    img[root_off : root_off + len(root_entries_raw)] = root_entries_raw

    for d in dirs:
        if d is root:
            continue
        raw = build_dir_entries(d, d.parent.first_cluster, taken_by_dir)
        if len(raw) > d._reserved * cluster_bytes:
            raise SystemExit("mkfat: directory too big: %s" % d.name)
        off = (data_start + (d.first_cluster - 2) * sec_per_clus) * SECTOR
        img[off : off + len(raw)] = raw

    # ---- files
    for f in files:
        off = (data_start + (f.first_cluster - 2) * sec_per_clus) * SECTOR
        img[off : off + len(f.data)] = f.data

    # ---- boot sector
    bs = bytearray(SECTOR)
    bs[0:3] = b"\xEB\x3C\x90"
    bs[3:11] = b"MSWIN4.1"
    struct.pack_into("<H", bs, 11, SECTOR)
    bs[13] = sec_per_clus
    struct.pack_into("<H", bs, 14, 1)                       # reserved sectors
    bs[16] = 2                                              # FATs
    struct.pack_into("<H", bs, 17, root_entries)
    if total_sectors < 0x10000:
        struct.pack_into("<H", bs, 19, total_sectors)
        struct.pack_into("<I", bs, 32, 0)
    else:
        struct.pack_into("<H", bs, 19, 0)
        struct.pack_into("<I", bs, 32, total_sectors)
    bs[21] = 0xF8                                           # media
    struct.pack_into("<H", bs, 22, fat_sectors)
    struct.pack_into("<H", bs, 24, 32)                      # sectors per track
    struct.pack_into("<H", bs, 26, 64)                      # heads
    struct.pack_into("<I", bs, 28, 0)                       # hidden
    struct.pack_into("<I", bs, 36, 0x12345678)              # volume serial
    lab = label.upper().encode("ascii", "replace")[:11].ljust(11)
    bs[43:54] = lab
    bs[54:62] = b"FAT12   " if fat_bits == 12 else b"FAT16   "
    bs[510:512] = b"\x55\xAA"
    img[0:SECTOR] = bs

    return bytes(img)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("srcdir")
    ap.add_argument("--label", default="WREBOOT")
    ap.add_argument("--size", type=int, default=None)
    args = ap.parse_args()

    if not os.path.isdir(args.srcdir):
        sys.exit("mkfat: %s is not a directory" % args.srcdir)
    root = scan(args.srcdir)
    root.name = "ROOT"
    data = build(root, args.label, args.size)
    with open(args.image, "wb") as f:
        f.write(data)
    print("mkfat: %s (%d KiB)" % (args.image, len(data) // 1024))


if __name__ == "__main__":
    main()
