#!/usr/bin/env python3
"""mksam.py - build a small but realistic SAM hive for tests.

Accounts:
  Administrator (RID 500)  enabled, password set, member of Administrators
  alice         (RID 1001) disabled, 3 failed logins, password set, admin

    mksam.py OUT.bin
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from mkhive import Hive  # noqa: E402

MACHINE_SID = bytes([1, 5, 0, 0, 0, 0, 0, 5])


def sid(rid):
    """S-1-5-21-1111-2222-3333-<rid> as the binary SAM form."""
    parts = [21, 1111, 2222, 3333, rid]
    return MACHINE_SID + b"".join(p.to_bytes(4, "little") for p in parts)


def hash_block(marker=b"\x11" * 16, length=20):
    """Old style SAM_HASH: 2 byte header, 2 byte prefix, 4 byte pad, 16 hash."""
    return b"\x01\x00" + b"\x00\x00" + b"\x00\x00\x00\x00" + marker + b"\x00" * 4


def user_v(name, fullname="", nt_len=20, lm_len=20):
    """Build a V record: 0xCC bytes of pointers, then the data."""
    uname = name.encode("utf-16-le")
    fname = fullname.encode("utf-16-le")
    lm = hash_block(b"\xaa" * 16, lm_len)
    nt = hash_block(b"\xbb" * 16, nt_len)

    data = b""
    nofs = len(data); data += uname
    fofs = len(data); data += fname
    lmofs = len(data); data += lm
    ntofs = len(data); data += nt

    hdr = bytearray(0xCC)
    hdr[0x0C:0x10] = nofs.to_bytes(4, "little")
    hdr[0x10:0x14] = len(uname).to_bytes(4, "little")
    hdr[0x18:0x1C] = fofs.to_bytes(4, "little")
    hdr[0x1C:0x20] = len(fname).to_bytes(4, "little")
    hdr[0x9C:0xA0] = lmofs.to_bytes(4, "little")
    hdr[0xA0:0xA4] = lm_len.to_bytes(4, "little")
    hdr[0xA8:0xAC] = ntofs.to_bytes(4, "little")
    hdr[0xAC:0xB0] = nt_len.to_bytes(4, "little")
    return bytes(hdr) + data


def user_f(rid, acb, failed=0):
    f = bytearray(0x50)
    f[0x30:0x34] = rid.to_bytes(4, "little")
    f[0x38:0x3A] = acb.to_bytes(2, "little")
    f[0x40:0x42] = failed.to_bytes(2, "little")
    return bytes(f)


def alias_c(groupname, member_rids):
    name = groupname.encode("utf-16-le")
    members = b"".join(sid(r) for r in member_rids)
    hdr = bytearray(0x34)
    hdr[0x00:0x04] = (0x220).to_bytes(4, "little")
    hdr[0x10:0x14] = (0).to_bytes(4, "little")
    hdr[0x14:0x18] = len(name).to_bytes(4, "little")
    hdr[0x1C:0x20] = len(name).to_bytes(4, "little")
    hdr[0x20:0x24] = (0).to_bytes(4, "little")
    hdr[0x28:0x2C] = len(name).to_bytes(4, "little")
    hdr[0x2C:0x30] = len(members).to_bytes(4, "little")
    hdr[0x30:0x34] = len(member_rids).to_bytes(4, "little")
    return bytes(hdr) + name + members


def build(path):
    h = Hive(root_name="SAM")

    users = [
        ("Administrator", 500, 0x0210, 0, "Built-in administrator"),
        ("alice", 1001, 0x0011, 3, "Alice Example"),
    ]
    for name, rid, acb, failed, fullname in users:
        h.set(r"Domains\Account\Users\Names\%s" % name, {
            name: ("binary", (0).to_bytes(4, "little") + (0).to_bytes(4, "little")
                   + rid.to_bytes(4, "little")),
        })
        h.set(r"Domains\Account\Users\%08X" % rid, {
            "F": ("binary", user_f(rid, acb, failed)),
            "V": ("binary", user_v(name, fullname)),
        })

    h.set(r"Domains\Builtin\Aliases\00000220", {
        "C": ("binary", alias_c("Administrators", [500, 1001])),
    })
    h.set(r"Domains\Account", {
        "F": ("binary", b"\x00" * 0xF0),
        "V": ("binary", b"\x00" * 0x100),
    })

    with open(path, "wb") as f:
        f.write(h.build())


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    build(sys.argv[1])
    print("mksam: %s" % sys.argv[1])