#!/usr/bin/env python3
"""mkinitramfs.py - pack a directory into a gzip'ed cpio (newc) initramfs.

Why not `find | cpio`: we need a handful of static device nodes (/dev/console
etc.) and stable ownership/metadata, and we must not require root on the build
host. Writing the newc archive directly is ~40 lines and fully deterministic.

    mkinitramfs.py ROOTDIR OUT.cpio.gz [--mtime EPOCH]
"""

import argparse
import gzip
import os
import stat
import sys

DEV_NODES = {
    "dev/console": (stat.S_IFCHR | 0o600, (5, 1)),
    "dev/null": (stat.S_IFCHR | 0o666, (1, 3)),
    "dev/zero": (stat.S_IFCHR | 0o666, (1, 5)),
    "dev/tty": (stat.S_IFCHR | 0o666, (5, 0)),
    "dev/urandom": (stat.S_IFCHR | 0o444, (1, 9)),
}

ALIGN = 4


def pad(buf):
    return buf + b"\x00" * ((ALIGN - len(buf) % ALIGN) % ALIGN)


def entry(name, mode, data=b"", ino=0, nlink=1, mtime=0, rdev=(0, 0)):
    namez = name.encode() + b"\x00"
    hdr = b"070701"
    fields = [
        ino,
        mode,
        0,          # uid
        0,          # gid
        nlink,
        mtime,
        len(data),
        0,          # devmajor
        0,          # devminor
        rdev[0],
        rdev[1],
        len(namez),
        0,          # check
    ]
    hdr += b"".join(b"%08x" % f for f in fields)
    return pad(hdr + namez) + pad(data)


def walk(root, mtime):
    out = []
    ino = 1

    def add(name, path):
        nonlocal ino
        st = os.lstat(path)
        if stat.S_ISDIR(st.st_mode):
            out.append(entry(name, stat.S_IFDIR | 0o755, ino=ino, mtime=mtime))
            ino += 1
            for child in sorted(os.listdir(path)):
                add(name.rstrip("/") + "/" + child if name != "." else child,
                    os.path.join(path, child))
        elif stat.S_ISLNK(st.st_mode):
            out.append(entry(name, stat.S_IFLNK | 0o777,
                             os.readlink(path).encode(), ino=ino,
                             mtime=mtime))
            ino += 1
        elif stat.S_ISREG(st.st_mode):
            with open(path, "rb") as f:
                data = f.read()
            mode = stat.S_IFREG | (st.st_mode & 0o777)
            out.append(entry(name, mode, data, ino=ino, mtime=mtime))
            ino += 1

    add(".", root)
    for node, (mode, rdev) in sorted(DEV_NODES.items()):
        out.append(entry(node, mode, ino=ino, mtime=mtime, rdev=rdev))
        ino += 1
    out.append(entry("TRAILER!!!", 0, ino=ino, mtime=mtime))
    return b"".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rootdir")
    ap.add_argument("output")
    ap.add_argument("--mtime", type=int, default=1700000000)
    args = ap.parse_args()

    if not os.path.isdir(args.rootdir):
        sys.exit("mkinitramfs: %s is not a directory" % args.rootdir)

    raw = walk(args.rootdir, args.mtime)
    with gzip.GzipFile(filename="", mode="wb", fileobj=open(args.output, "wb"),
                       compresslevel=9, mtime=args.mtime) as gz:
        gz.write(raw)
    print("mkinitramfs: %s (%d KiB raw, %d KiB gz)" %
          (args.output, len(raw) // 1024, os.path.getsize(args.output) // 1024))


if __name__ == "__main__":
    main()