#!/usr/bin/env python3
"""mkgpt.py - build a GPT disk image from several filesystem/raw images.

Test helper, big brother of mkdiskimg.py: a real ESXi host has the boot
partitions and the datastores on one disk, so the tests need a disk with more
than one partition.

    mkgpt.py --out disk.img --part a.fs --part b.fs [--part c.raw] \
             [--size-mb N] [--gap-mb 1]

Every partition is typed "Microsoft basic data"; Linux does not look at the
type when mounting, and blkid reads the label out of the filesystem itself.
No root, no loop devices.
"""

import argparse
import binascii
import struct
import sys
import uuid

SECTOR = 512
ENTRIES = 128
ENTRY_SIZE = 128
ENTRIES_SECTORS = ENTRIES * ENTRY_SIZE // SECTOR

BASIC_DATA = uuid.UUID("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--part", action="append", required=True,
                    help="bare filesystem or raw image (repeatable)")
    ap.add_argument("--size-mb", type=int, default=None,
                    help="pad the disk to this size")
    ap.add_argument("--gap-mb", type=int, default=1,
                    help="free space between partitions (default 1 MiB)")
    args = ap.parse_args()

    parts = []
    for path in args.part:
        with open(path, "rb") as f:
            data = f.read()
        if len(data) % SECTOR:
            sys.exit("mkgpt: %s is not sector aligned" % path)
        parts.append(data)

    gap = args.gap_mb * 1024 * 1024 // SECTOR
    start = 2 + ENTRIES_SECTORS + gap
    layout = []
    for data in parts:
        sectors = len(data) // SECTOR
        layout.append((start, sectors, data))
        start += sectors + gap
    total = start + ENTRIES_SECTORS + 1
    if args.size_mb:
        total = max(total, args.size_mb * 1024 * 1024 // SECTOR)

    img = bytearray(total * SECTOR)
    disk_guid = uuid.uuid4()

    # protective MBR
    mbr = img[0:SECTOR]
    mbr[446 + 4] = 0xEE
    mbr[446 + 5] = 0xFF
    mbr[446 + 6] = 0xFF
    mbr[446 + 7] = 0xFF
    struct.pack_into("<I", mbr, 446 + 8, 1)
    struct.pack_into("<I", mbr, 446 + 12, min(total - 1, 0xFFFFFFFF))
    mbr[510:512] = b"\x55\xAA"
    img[0:SECTOR] = mbr

    table = bytearray(ENTRIES * ENTRY_SIZE)
    for i, (first, sectors, data) in enumerate(layout):
        off = i * ENTRY_SIZE
        struct.pack_into("<16s", table, off, BASIC_DATA.bytes_le)
        struct.pack_into("<16s", table, off + 16, uuid.uuid4().bytes_le)
        struct.pack_into("<Q", table, off + 32, first)
        struct.pack_into("<Q", table, off + 40, first + sectors - 1)
        struct.pack_into("<Q", table, off + 48, 0)
        img[first * SECTOR:first * SECTOR + len(data)] = data

    table_crc = binascii.crc32(bytes(table)) & 0xFFFFFFFF

    def header(my_lba, alt_lba, entries_lba):
        h = bytearray(SECTOR)
        h[0:8] = b"EFI PART"
        struct.pack_into("<I", h, 8, 0x00010000)
        struct.pack_into("<I", h, 12, 92)
        struct.pack_into("<Q", h, 24, my_lba)
        struct.pack_into("<Q", h, 32, alt_lba)
        struct.pack_into("<Q", h, 40, 2 + ENTRIES_SECTORS)
        struct.pack_into("<Q", h, 48, total - 2 - ENTRIES_SECTORS)
        h[56:72] = disk_guid.bytes_le
        struct.pack_into("<Q", h, 72, entries_lba)
        struct.pack_into("<I", h, 80, ENTRIES)
        struct.pack_into("<I", h, 84, ENTRY_SIZE)
        struct.pack_into("<I", h, 88, table_crc)
        struct.pack_into("<I", h, 16,
                         binascii.crc32(bytes(h[0:16] + h[20:92])) & 0xFFFFFFFF)
        return bytes(h)

    img[SECTOR:2 * SECTOR] = header(1, total - 1, 2)
    img[2 * SECTOR:2 * SECTOR + len(table)] = table

    backup_lba = total - 1 - ENTRIES_SECTORS
    img[backup_lba * SECTOR:backup_lba * SECTOR + len(table)] = table
    img[(total - 1) * SECTOR:total * SECTOR] = header(total - 1, 1, backup_lba)

    with open(args.out, "wb") as f:
        f.write(img)
    print("mkgpt: %s (%d MiB, %d partitions)" %
          (args.out, len(img) // (1024 * 1024), len(layout)))


if __name__ == "__main__":
    main()
