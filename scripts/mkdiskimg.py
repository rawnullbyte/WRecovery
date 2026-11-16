#!/usr/bin/env python3
"""mkdiskimg.py - wrap a filesystem image in a partitioned disk image.

Test helper: takes a bare filesystem image (e.g. made by mkntfs) and produces
a disk image with a DOS/MBR partition table containing it, so the guest sees
/dev/sda1 the way it would on a real Windows disk.

    mkdiskimg.py --fs ntfs.img --out disk.img [--offset-mb 1] [--type 7]
                 [--table mbr|gpt]

No root, no loop devices.
"""

import argparse
import os
import struct
import sys

SECTOR = 512


def build_mbr(total_sectors, start_lba, part_sectors, ptype):
    mbr = bytearray(SECTOR)
    # partition entry 1
    e = 446
    mbr[e + 0] = 0x80              # bootable
    mbr[e + 1] = 0x00              # CHS start (ignored by modern parsers)
    mbr[e + 2] = 0x02
    mbr[e + 3] = 0x00
    mbr[e + 4] = ptype
    mbr[e + 5] = 0xFE
    mbr[e + 6] = 0xFF
    mbr[e + 7] = 0xFF
    struct.pack_into("<I", mbr, e + 8, start_lba)
    struct.pack_into("<I", mbr, e + 12, part_sectors)
    mbr[510:512] = b"\x55\xAA"
    return mbr


def build_gpt(total_sectors, start_lba, part_sectors):
    """Minimal protective MBR + GPT with one Microsoft basic data partition."""
    import uuid as uuidlib

    disk_guid = uuidlib.UUID("5f8a9c1e-1111-4222-8333-944455556666")
    part_guid = uuidlib.UUID("aabbccdd-1234-5678-9abc-def012345678")
    basic_data = uuidlib.UUID("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7")

    # protective MBR
    mbr = bytearray(SECTOR)
    e = 446
    mbr[e + 4] = 0xEE
    mbr[e + 5] = 0xFF
    mbr[e + 6] = 0xFF
    mbr[e + 7] = 0xFF
    struct.pack_into("<I", mbr, e + 8, 1)
    struct.pack_into("<I", mbr, e + 12, min(total_sectors - 1, 0xFFFFFFFF))
    mbr[510:512] = b"\x55\xAA"

    # header
    entries = 128
    entry_size = 128
    entries_sectors = entries * entry_size // SECTOR
    header = bytearray(SECTOR)
    header[0:8] = b"EFI PART"
    struct.pack_into("<I", header, 8, 0x00010000)
    struct.pack_into("<I", header, 12, 92)
    struct.pack_into("<Q", header, 24, 1)                      # my LBA
    struct.pack_into("<Q", header, 32, total_sectors - 1)      # alternate
    struct.pack_into("<Q", header, 40, 2 + entries_sectors)    # first usable
    struct.pack_into("<Q", header, 48, total_sectors - 2 - entries_sectors)
    header[56:72] = disk_guid.bytes_le
    struct.pack_into("<Q", header, 72, 2)                      # entries LBA
    struct.pack_into("<I", header, 80, entries)
    struct.pack_into("<I", header, 84, entry_size)
    struct.pack_into("<I", header, 88, 0)
    import binascii
    struct.pack_into("<I", header, 16, binascii.crc32(bytes(header[0:16] + header[20:92])) & 0xFFFFFFFF)

    table = bytearray(entries * entry_size)
    struct.pack_into("<16s", table, 0, basic_data.bytes_le)
    struct.pack_into("<16s", table, 16, part_guid.bytes_le)
    struct.pack_into("<Q", table, 32, start_lba)
    struct.pack_into("<Q", table, 40, start_lba + part_sectors - 1)
    struct.pack_into("<Q", table, 48, 0)

    return bytes(mbr), bytes(header), bytes(table), entries_sectors


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fs", required=True, help="bare filesystem image")
    ap.add_argument("--out", required=True, help="disk image to write")
    ap.add_argument("--offset-mb", type=int, default=1)
    ap.add_argument("--type", type=lambda x: int(x, 0), default=0x07)
    ap.add_argument("--table", choices=("mbr", "gpt"), default="mbr")
    ap.add_argument("--size-mb", type=int, default=None)
    args = ap.parse_args()

    with open(args.fs, "rb") as f:
        fs = f.read()
    if len(fs) % SECTOR:
        sys.exit("mkdiskimg: filesystem image is not sector aligned")

    start_lba = args.offset_mb * 1024 * 1024 // SECTOR
    part_sectors = len(fs) // SECTOR
    total_sectors = start_lba + part_sectors + 2048
    if args.size_mb:
        total_sectors = max(total_sectors, args.size_mb * 1024 * 1024 // SECTOR)

    img = bytearray(total_sectors * SECTOR)

    if args.table == "mbr":
        img[0:SECTOR] = build_mbr(total_sectors, start_lba, part_sectors, args.type)
    else:
        mbr, header, table, entries_sectors = build_gpt(total_sectors, start_lba, part_sectors)
        img[0:SECTOR] = mbr
        img[SECTOR:2 * SECTOR] = header
        img[2 * SECTOR:2 * SECTOR + len(table)] = table
        # backup entries + header at the end
        backup_entries_lba = total_sectors - 1 - entries_sectors
        img[backup_entries_lba * SECTOR:backup_entries_lba * SECTOR + len(table)] = table
        bh = bytearray(header)
        struct.pack_into("<Q", bh, 24, total_sectors - 1)
        struct.pack_into("<Q", bh, 32, 1)
        struct.pack_into("<Q", bh, 72, backup_entries_lba)
        import binascii
        struct.pack_into("<I", bh, 16, binascii.crc32(bytes(bh[0:16] + bh[20:92])) & 0xFFFFFFFF)
        img[(total_sectors - 1) * SECTOR:total_sectors * SECTOR] = bh

    off = start_lba * SECTOR
    img[off:off + len(fs)] = fs

    with open(args.out, "wb") as f:
        f.write(img)
    print("mkdiskimg: %s (%d MiB, %s, partition at LBA %d, %d sectors)" %
          (args.out, len(img) // (1024 * 1024), args.table, start_lba, part_sectors))


if __name__ == "__main__":
    main()