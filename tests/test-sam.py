#!/usr/bin/env python3
"""test-sam.py - check that `winreg sam-reset` produced a usable SAM hive.

Reads the patched hive back with python-registry (an independent
implementation) and verifies:
  * the LM and NT hash lengths are zero (blank password, ntpasswd style),
  * the account is enabled and the failed-logon counter is reset,
  * the base block checksum matches the data (Windows refuses hives with a
    bad checksum).

    test-sam.py SAM_BEFORE SAM_AFTER RID
"""

import struct
import sys

from Registry import Registry

ACB_DISABLED = 0x0001
ACB_AUTOLOCK = 0x0400


def check_checksum(path):
    with open(path, "rb") as f:
        hdr = f.read(0x200)
    stored = struct.unpack_from("<I", hdr, 0x1FC)[0]
    total = 0
    for i in range(0, 0x1FC, 4):
        total ^= struct.unpack_from("<I", hdr, i)[0]
    return stored == total, stored, total


def read_state(path, rid):
    reg = Registry.Registry(path)
    key = reg.open(r"Domains\Account\Users\%08X" % rid)
    v = key.value("V").value()
    f = key.value("F").value()
    return {
        "lm_len": struct.unpack_from("<I", v, 0xA0)[0],
        "nt_len": struct.unpack_from("<I", v, 0xAC)[0],
        "acb": struct.unpack_from("<H", f, 0x38)[0],
        "failed": struct.unpack_from("<H", f, 0x40)[0],
    }


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    before, after, rid = sys.argv[1], sys.argv[2], int(sys.argv[3])

    b = read_state(before, rid)
    a = read_state(after, rid)
    problems = []

    if b["nt_len"] == 0:
        problems.append("fixture already had a blank password")
    if a["nt_len"] != 0 or a["lm_len"] != 0:
        problems.append("hash lengths not cleared: %r" % a)
    if a["acb"] & ACB_DISABLED:
        problems.append("account still disabled (ACB=0x%04x)" % a["acb"])
    if a["acb"] & ACB_AUTOLOCK:
        problems.append("account still locked (ACB=0x%04x)" % a["acb"])
    if a["failed"] != 0:
        problems.append("failed logon counter not reset (%d)" % a["failed"])

    ok, stored, calc = check_checksum(after)
    if not ok:
        problems.append("base block checksum wrong: stored 0x%08x, data 0x%08x"
                        % (stored, calc))

    for p in problems:
        print("  FAIL %s" % p)
    if problems:
        return 1
    print("  ok   password cleared (lm_len=0, nt_len=0), ACB=0x%04x, "
          "failed=0, checksum ok" % a["acb"])
    return 0


if __name__ == "__main__":
    sys.exit(main())